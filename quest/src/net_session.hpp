#pragma once
// NetSession — the Quest client's transport + session pump.
//
// Owns the QUIC transport, the shared loom::core::Session handshake state
// machine, and the shared loom::core::VideoReceiver. pump() is called once per
// render frame: it drains transport events into Session and the receiver, drives
// clock-sync, and sends the control frames Session produces. This is the SDL
// client's single-threaded main loop (sdl/src/main.cpp), run on the OpenXR
// render thread — exactly the threading model chosen for M3.3.
//
// Decode is elsewhere: the AMediaCodec decode thread pops access units from
// receiver(); nothing about decode runs here.

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "loom/core/session.hpp"
#include "loom/core/video_receiver.hpp"
#include "quic_transport.hpp"

namespace loom::quest {

class NetSession {
 public:
  explicit NetSession(loom::core::HelloParams params);

  bool start(const std::string& host, std::uint16_t port);

  // Drain transport events, drive Session + clock sync + the receiver, send
  // control frames. `now_us` is the client monotonic clock (§7). Call per frame.
  void pump(std::int64_t now_us);

  // Newly reached STREAMING this pump (START received) — the app brings the
  // decoder up on the first true.
  bool just_started_streaming() const { return just_started_streaming_; }
  // A mid-session CONFIG was applied this pump (§8) — the app tears down and
  // recreates the decoder at the new config().
  bool config_changed() const { return config_changed_; }
  bool streaming() const { return streaming_; }
  bool finished() const { return closed_ || fatal_; }

  // Request the host stream at `width`x`height` (VIEWPORT, §3.10). Thread-safe:
  // called from the UI thread; the pending size is forwarded to Session on the
  // next pump (which enforces the §3.10 rate limit). Coalesces: only the most
  // recent size matters.
  void request_viewport(std::uint32_t width, std::uint32_t height) {
    requested_viewport_.store((static_cast<std::uint64_t>(width) << 32) | height,
                              std::memory_order_relaxed);
  }

  const std::optional<loom::core::SessionConfig>& config() const { return session_.config(); }
  std::optional<loom::proto::clocksync::Estimate> clock() const { return session_.clock(); }

  // The primary stream's receiver (stream_id 0) — always present.
  loom::core::VideoReceiver& receiver() { return *receivers_.front().second; }
  // The receiver for `stream_id`, or nullptr if that stream isn't served.
  loom::core::VideoReceiver* receiver(std::uint16_t stream_id) {
    for (auto& [sid, r] : receivers_)
      if (sid == stream_id) return r.get();
    return nullptr;
  }
  // Every served stream_id, primary first — the app iterates this to fan out one
  // decoder + one layer per stream (§3.4 multi-display).
  std::vector<std::uint16_t> stream_ids() const {
    std::vector<std::uint16_t> ids;
    ids.reserve(receivers_.size());
    for (const auto& [sid, r] : receivers_) ids.push_back(sid);
    return ids;
  }

  // Build and send a STATS report (§3.7). The app supplies the decode/frame
  // counts it owns; rtt/e2e come from the clock. `in.stream_id` scopes it.
  void send_stats(const loom::core::StatsInput& in);

 private:
  // Append a receiver for `stream_id`, wiring its IDR callback to the transport.
  void add_receiver(std::uint16_t stream_id);
  // Create the extra-display receivers once CONFIG (key 6) has been parsed and
  // multi-display is active. Idempotent; the primary receiver is built in the ctor.
  void build_extra_receivers();

  QuicTransport transport_;
  loom::core::Session session_;
  // One receiver per served video stream, primary (stream_id 0) at front. Held by
  // pointer because VideoReceiver is non-movable (owns a mutex/cv).
  std::vector<std::pair<std::uint16_t, std::unique_ptr<loom::core::VideoReceiver>>> receivers_;
  bool extra_receivers_built_ = false;

  bool streaming_ = false;
  bool just_started_streaming_ = false;
  bool config_changed_ = false;
  bool closed_ = false;
  bool fatal_ = false;

  // VIEWPORT handoff: UI thread stores (width<<32 | height); the pump reads it,
  // and `sent_viewport_` dedups so a settled size isn't re-sent every frame.
  std::atomic<std::uint64_t> requested_viewport_{0};
  std::uint64_t sent_viewport_ = 0;
};

}  // namespace loom::quest
