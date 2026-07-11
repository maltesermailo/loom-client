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

namespace loom::core {

// Coarse session phase (a subset of ARCHITECTURE §6.4, enough for M1.1).
enum class State {
  Connecting,   // constructed; awaiting the transport's Connected event
  Negotiating,  // HELLO sent; exchanging WELCOME/CONFIG/START
  Streaming,    // START received; media expected
  Closed,       // clean teardown (BYE)
  Failed,       // fatal ERROR or protocol violation
};

// Capabilities advertised in HELLO (PROTOCOL.md §3.4).
struct HelloParams {
  std::string client_name = "loom-client";
  std::vector<std::uint64_t> codecs = {1};  // preference-ordered; 1 = HEVC
  std::uint32_t max_width = 2560;
  std::uint32_t max_height = 1440;
  std::uint32_t max_refresh = 90;
  std::uint64_t features = 0;  // bit0 = audio playback; 0 for now (audio is M5)
};

// Media description parsed from the host's CONFIG (PROTOCOL.md §3.4).
struct SessionConfig {
  std::uint64_t generation = 0;
  std::uint64_t codec = 0;
  std::uint64_t width = 0;
  std::uint64_t height = 0;
  std::uint64_t refresh = 0;
  std::uint64_t audio = 0;
  std::uint64_t bitrate_kbps = 0;
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
    SendControl,   // write `bytes` (a complete length-prefixed frame) to control
    Established,   // WELCOME received (host identified)
    MediaExpected, // START received; bring up the decoder/media path (M1.2)
    Fatal,         // fatal: `code` is a PROTOCOL.md §10 error code
    Closed,        // clean close
  };
  Kind kind;
  std::vector<std::uint8_t> bytes;  // valid for SendControl
  std::uint64_t code = 0;           // valid for Fatal
};

class Session {
public:
  explicit Session(HelloParams params);

  // --- inputs (transport-agnostic) ---
  // Feed raw control-stream bytes; may be partial or span several frames. The
  // session buffers and extracts complete length-prefixed frames itself.
  void on_control_bytes(std::span<const std::uint8_t> bytes);
  void on_event(Event ev);

  // --- output ---
  // Drain the actions produced since the last poll(), in occurrence order.
  std::vector<Action> poll();

  // --- observers ---
  State state() const { return state_; }
  const std::optional<std::string>& host_name() const { return host_name_; }
  const std::optional<SessionConfig>& config() const { return config_; }

private:
  enum class Step { Welcome, Config, Start };  // next expected setup message

  void handle_frame(std::span<const std::uint8_t> frame);
  void fatal(std::uint64_t code);
  void push(Action a);

  HelloParams params_;
  State state_ = State::Connecting;
  Step step_ = Step::Welcome;
  std::vector<std::uint8_t> rx_;  // control-stream reassembly buffer
  std::vector<Action> out_;
  std::optional<std::string> host_name_;
  std::optional<SessionConfig> config_;
};

}  // namespace loom::core
