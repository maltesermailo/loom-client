#pragma once
// Client session state machine — the mirror of loomd's handshake
// (PROTOCOL.md §1.1, §3.4) from the client's side:
//
//   Connected -> send HELLO -> WELCOME -> CONFIG -> send CONFIG_ACK -> START
//   -> Streaming;  ERROR -> Failed;  BYE -> Closed.
//
// It is deliberately **sans-io and transport-agnostic**: feed it raw
// control-stream bytes and lifecycle events, drain Actions (bytes to send,
// milestones, teardown). It owns no sockets and knows nothing about QUIC, so
// the SDL client (msquic on desktop) and the Quest client (msquic on Android)
// drive the *same* object with different transports. Wire encode/decode is
// delegated to loom::proto — this class never touches a CBOR byte itself.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "loom/proto/clocksync.hpp"

namespace loom::core {

// Coarse session phase (a subset of ARCHITECTURE §6.4, enough for M1.1).
enum class State {
  Connecting,   // constructed; awaiting the transport's Connected event
  Negotiating,  // HELLO sent; exchanging WELCOME/CONFIG/START
  Streaming,    // START received; media expected
  Closed,       // clean teardown (BYE)
  Failed,       // fatal ERROR or protocol violation
};

// HELLO key 5 / WELCOME key 3 feature bits (PROTOCOL.md §3.4).
inline constexpr std::uint64_t kFeatureAudioPlayback = 0b01;  // bit 0 (audio is M5)
inline constexpr std::uint64_t kFeatureMultiDisplay = 0b10;   // bit 1: fan in N streams

// Capabilities advertised in HELLO (PROTOCOL.md §3.4).
struct HelloParams {
  std::string client_name = "loom-client";
  std::vector<std::uint64_t> codecs = {1};  // preference-ordered; 1 = HEVC
  std::uint32_t max_width = 2560;
  std::uint32_t max_height = 1440;
  std::uint32_t max_refresh = 90;
  // Feature bitmask. A client that can decode+display concurrent streams sets
  // kFeatureMultiDisplay; the host then fans out (CONFIG key 6). Default 0 keeps
  // a single-stream session, bit-exact with a pre-feature peer.
  std::uint64_t features = 0;
};

// One additional video stream the host will send beyond the primary — a display
// carried on its own stream_id (PROTOCOL.md §3.4 CONFIG key 6, multi-display).
struct StreamConfig {
  std::uint16_t stream_id = 0;  // >= 2 (0 is the primary, 1 is audio)
  std::uint64_t width = 0;
  std::uint64_t height = 0;
  std::uint64_t refresh = 0;
  std::uint64_t bitrate_kbps = 0;
};

// Media description parsed from the host's CONFIG (PROTOCOL.md §3.4). Keys 0-5
// describe the primary video stream (stream_id 0) + audio; `extra_streams` is
// CONFIG key 6, non-empty only when multi-display was negotiated.
struct SessionConfig {
  std::uint64_t generation = 0;
  std::uint64_t codec = 0;
  std::uint64_t width = 0;
  std::uint64_t height = 0;
  std::uint64_t refresh = 0;
  std::uint64_t audio = 0;
  std::uint64_t bitrate_kbps = 0;
  std::vector<StreamConfig> extra_streams;
};

// A lifecycle event fed in by the transport driver.
enum class Event {
  Connected,       // QUIC handshake done + control stream open: send HELLO
  ConnectionLost,  // transport dropped (reconnect policy is caller-local)
  UserBye,         // local user asked to end the session
};

// An instruction produced by the state machine, drained via poll().
struct Action {
  enum class Kind {
    SendControl,    // write `bytes` (a complete length-prefixed frame) to control
    Established,    // WELCOME received (host identified)
    MediaExpected,  // START received; bring up the decoder/media path (M1.2)
    ConfigChanged,  // mid-session CONFIG applied (§8); recreate the decoder at config()
    Fatal,          // fatal: `code` is a PROTOCOL.md §10 error code
    Closed,         // clean close
  };
  Kind kind;
  std::vector<std::uint8_t> bytes;  // valid for SendControl
  std::uint64_t code = 0;           // valid for Fatal
};

// Inputs the app supplies to build a STATS report (§3.7). rtt/e2e come from the
// clock; the frame/decode/loss counts come from the app's own instrumentation.
struct StatsInput {
  std::uint64_t frames_received = 0;
  std::uint64_t frames_dropped = 0;
  std::uint64_t datagrams = 0;
  double jitter_ms = 0.0;
  std::uint64_t decode_us = 0;
  std::uint64_t rtt_us = 0;
  std::optional<std::uint64_t> e2e_us;  // omitted before the first clock sample
};

class Session {
 public:
  explicit Session(HelloParams params);

  // --- inputs (transport-agnostic) ---
  // Feed raw control-stream bytes; may be partial or span several frames.
  // `now_us` is the client-clock arrival time (used to close CLOCK_PONG, §7).
  void on_control_bytes(std::span<const std::uint8_t> bytes, std::int64_t now_us = 0);
  void on_event(Event ev);
  // Time tick (main loop drives it): queues a CLOCK_PING every 500 ms (§3.8),
  // drained via poll() like every other action.
  void on_tick(std::int64_t now_us);

  // --- output ---
  // Drain the actions produced since the last poll(), in occurrence order.
  std::vector<Action> poll();

  // Build a STATS frame (§3.7) to send on the control stream.
  std::vector<std::uint8_t> encode_stats(const StatsInput& in) const;

  // Request that the host stream at `width`x`height` (VIEWPORT, §3.10) so the
  // decoded video maps ~1:1 to the client's window. Best-effort: the host may
  // clamp or ignore it. Only valid while STREAMING; rate-limited to at most one
  // per 250 ms (§3.10) — a suppressed call is a no-op. Returns whether a frame
  // was queued (drain it via poll() like every other action).
  bool send_viewport(std::uint32_t width, std::uint32_t height, std::int64_t now_us);

  // --- observers ---
  State state() const { return state_; }
  const std::optional<std::string>& host_name() const { return host_name_; }
  const std::optional<SessionConfig>& config() const { return config_; }
  // Feature bits the host activated for this session (WELCOME key 3, §3.4).
  // Test bit kFeatureMultiDisplay to decide whether to fan in extra streams.
  std::uint64_t features() const { return active_features_; }
  // Current clock estimate (rtt/offset µs), or nullopt before the first sample.
  std::optional<proto::clocksync::Estimate> clock() const { return clock_estimate_; }

 private:
  enum class Step { Welcome, Config, Start };  // next expected setup message

  void handle_frame(std::span<const std::uint8_t> frame, std::int64_t now_us);
  void send_config_ack(std::uint64_t generation);
  void fatal(std::uint64_t code);
  void push(Action a);

  HelloParams params_;
  State state_ = State::Connecting;
  Step step_ = Step::Welcome;
  std::vector<std::uint8_t> rx_;  // control-stream reassembly buffer
  std::vector<Action> out_;
  std::optional<std::string> host_name_;
  std::optional<SessionConfig> config_;
  std::uint64_t active_features_ = 0;  // WELCOME key 3 (§3.4)

  // Clock sync (§7): the min-filter lives in proto; we only wire it.
  proto::clocksync::ClockFilter clock_filter_;
  std::optional<proto::clocksync::Estimate> clock_estimate_;
  std::int64_t last_ping_us_ = 0;
  bool pinged_ = false;

  // VIEWPORT rate limit (§3.10): at most one per 250 ms.
  std::int64_t last_viewport_us_ = 0;
  bool viewport_sent_ = false;
};

}  // namespace loom::core
