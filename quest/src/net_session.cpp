#include "net_session.hpp"

#include "log.hpp"
#include "loom/proto/control.hpp"

namespace loom::quest {
namespace {

namespace control = loom::proto::control;
using loom::core::Action;
using loom::core::Event;
using loom::proto::cbor::Value;

// IDR_REQUEST frame (§3.6): body {0: last_good_frame_seq}.
std::vector<std::uint8_t> idr_request_frame(std::uint32_t last_good) {
  Value::Map body;
  body.emplace_back(Value::integer(0), Value::integer(static_cast<std::int64_t>(last_good)));
  return control::encode_frame(control::kIdrRequest, body);
}

}  // namespace

NetSession::NetSession(loom::core::HelloParams params)
    : session_(std::move(params)),
      // Every IDR request the receiver raises goes straight out the control
      // stream; send_control no-ops until the control stream is open.
      receiver_([this](std::uint32_t last_good) {
        transport_.send_control(idr_request_frame(last_good));
      }) {}

bool NetSession::start(const std::string& host, std::uint16_t port) {
  return transport_.start(host, port);
}

void NetSession::pump(std::int64_t now_us) {
  just_started_streaming_ = false;

  while (auto ev = transport_.next_event()) {
    switch (ev->kind) {
      case TransportEvent::Kind::Connected:
        session_.on_event(Event::Connected);
        break;
      case TransportEvent::Kind::ControlBytes:
        session_.on_control_bytes(ev->bytes, now_us);
        break;
      case TransportEvent::Kind::Datagram:
        receiver_.feed_datagram(ev->bytes, now_us / 1000);
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
