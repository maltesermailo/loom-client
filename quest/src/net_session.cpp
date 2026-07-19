#include "net_session.hpp"

#include "log.hpp"
#include "loom/proto/control.hpp"

namespace loom::quest {
namespace {

namespace control = loom::proto::control;
using loom::core::Action;
using loom::core::Event;
using loom::proto::cbor::Value;

// IDR_REQUEST frame (§3.6): body {0: last_good_frame_seq, 1: stream_id}. Key 1 is
// omitted for the primary so a single-stream request is byte-identical to before.
std::vector<std::uint8_t> idr_request_frame(std::uint32_t last_good, std::uint16_t stream_id) {
  Value::Map body;
  body.emplace_back(Value::integer(0), Value::integer(static_cast<std::int64_t>(last_good)));
  if (stream_id != 0) {
    body.emplace_back(Value::integer(1), Value::integer(stream_id));
  }
  return control::encode_frame(control::kIdrRequest, body);
}

}  // namespace

NetSession::NetSession(loom::core::HelloParams params) : session_(std::move(params)) {
  // The primary stream (stream_id 0) always exists; extra displays are added once
  // CONFIG key 6 arrives (build_extra_receivers). Each receiver's IDR requests go
  // straight out the control stream, tagged with its stream_id (§3.6).
  add_receiver(0);
}

void NetSession::add_receiver(std::uint16_t stream_id) {
  receivers_.emplace_back(stream_id,
                          std::make_unique<loom::core::VideoReceiver>(
                              [this, stream_id](std::uint32_t last_good) {
                                transport_.send_control(idr_request_frame(last_good, stream_id));
                              },
                              stream_id));
}

void NetSession::build_extra_receivers() {
  if (extra_receivers_built_) return;
  const auto& cfg = session_.config();
  if (!cfg) return;  // no CONFIG yet
  extra_receivers_built_ = true;
  if ((session_.features() & loom::core::kFeatureMultiDisplay) == 0) return;  // single-stream
  for (const auto& s : cfg->extra_streams) add_receiver(s.stream_id);
}

bool NetSession::start(const std::string& host, std::uint16_t port) {
  return transport_.start(host, port);
}

void NetSession::pump(std::int64_t now_us) {
  just_started_streaming_ = false;
  config_changed_ = false;

  while (auto ev = transport_.next_event()) {
    switch (ev->kind) {
      case TransportEvent::Kind::Connected:
        session_.on_event(Event::Connected);
        break;
      case TransportEvent::Kind::ControlBytes:
        session_.on_control_bytes(ev->bytes, now_us);
        // Once CONFIG is parsed, stand up the extra-display receivers so they are
        // ready before any of their datagrams arrive (§3.4).
        build_extra_receivers();
        break;
      case TransportEvent::Kind::Datagram:
        // Feed every receiver; each keeps only its own stream_id (M6.3 fan-in).
        for (auto& [sid, r] : receivers_) r->feed_datagram(ev->bytes, now_us / 1000);
        break;
      case TransportEvent::Kind::Closed:
        if (ev->code != 0) {
          LOOM_LOGE("connection closed by host, code 0x%02llx",
                    static_cast<unsigned long long>(ev->code));
        }
        closed_ = true;
        break;
    }
  }

  session_.on_tick(now_us);  // queues a CLOCK_PING when due (§3.8)

  // Emit a pending VIEWPORT request (§3.10). Retry until Session accepts it
  // (its rate limit may suppress the first attempt), then dedup on the sent size.
  const std::uint64_t req = requested_viewport_.load(std::memory_order_relaxed);
  if (req != 0 && req != sent_viewport_ &&
      session_.send_viewport(static_cast<std::uint32_t>(req >> 32),
                             static_cast<std::uint32_t>(req & 0xffffffffu), now_us)) {
    sent_viewport_ = req;
  }

  for (const auto& a : session_.poll()) {
    switch (a.kind) {
      case Action::Kind::SendControl:
        transport_.send_control(a.bytes);
        break;
      case Action::Kind::Established:
        LOOM_LOGI("WELCOME from \"%s\"",
                  session_.host_name() ? session_.host_name()->c_str() : "?");
        break;
      case Action::Kind::MediaExpected:
        streaming_ = true;
        just_started_streaming_ = true;
        break;
      case Action::Kind::ConfigChanged:
        config_changed_ = true;
        break;
      case Action::Kind::Fatal:
        LOOM_LOGE("fatal session error, code 0x%02llx", static_cast<unsigned long long>(a.code));
        fatal_ = true;
        break;
      case Action::Kind::Closed:
        closed_ = true;
        break;
    }
  }
}

void NetSession::send_stats(const loom::core::StatsInput& in) {
  transport_.send_control(session_.encode_stats(in));
}

}  // namespace loom::quest
