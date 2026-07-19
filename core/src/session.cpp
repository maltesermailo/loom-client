#include "loom/core/session.hpp"

#include <cstdint>

#include "loom/proto/cbor.hpp"
#include "loom/proto/control.hpp"
#include "loom/proto/errors.hpp"

namespace loom::core {
namespace {

using loom::proto::cbor::Value;
namespace control = loom::proto::control;
namespace errors = loom::proto::errors;

std::optional<std::int64_t> find_int(const Value::Map& m, std::int64_t key) {
  for (const auto& [k, v] : m) {
    if (k.type() == Value::Type::Int && k.as_int() == key && v.type() == Value::Type::Int) {
      return v.as_int();
    }
  }
  return std::nullopt;
}

std::optional<std::string> find_text(const Value::Map& m, std::int64_t key) {
  for (const auto& [k, v] : m) {
    if (k.type() == Value::Type::Int && k.as_int() == key && v.type() == Value::Type::Text) {
      return v.as_text();
    }
  }
  return std::nullopt;
}

// Element `idx` of a 2-int array body value (e.g. CONFIG key 2 = [w, h]).
std::optional<std::int64_t> find_pair_elem(const Value::Map& m, std::int64_t key, std::size_t idx) {
  for (const auto& [k, v] : m) {
    if (k.type() == Value::Type::Int && k.as_int() == key && v.type() == Value::Type::Array) {
      const auto& a = v.as_array();
      if (idx < a.size() && a[idx].type() == Value::Type::Int) return a[idx].as_int();
    }
  }
  return std::nullopt;
}

// The Array value at integer `key`, or nullptr (e.g. CONFIG key 6 = streams).
const Value::Array* find_array(const Value::Map& m, std::int64_t key) {
  for (const auto& [k, v] : m) {
    if (k.type() == Value::Type::Int && k.as_int() == key && v.type() == Value::Type::Array) {
      return &v.as_array();
    }
  }
  return nullptr;
}

// Parse a CONFIG (0x03) body into a SessionConfig (§3.4). Missing keys default
// to 0, matching the ignore-unknown / best-effort decode elsewhere. Key 6 lists
// the additional video streams (multi-display); absent ⇒ single-stream.
SessionConfig parse_config(const Value::Map& body) {
  SessionConfig c;
  c.generation = static_cast<std::uint64_t>(find_int(body, 0).value_or(0));
  c.codec = static_cast<std::uint64_t>(find_int(body, 1).value_or(0));
  c.width = static_cast<std::uint64_t>(find_pair_elem(body, 2, 0).value_or(0));
  c.height = static_cast<std::uint64_t>(find_pair_elem(body, 2, 1).value_or(0));
  c.refresh = static_cast<std::uint64_t>(find_int(body, 3).value_or(0));
  c.audio = static_cast<std::uint64_t>(find_int(body, 4).value_or(0));
  c.bitrate_kbps = static_cast<std::uint64_t>(find_int(body, 5).value_or(0));

  if (const auto* streams = find_array(body, 6)) {
    for (const auto& s : *streams) {
      if (s.type() != Value::Type::Map) continue;  // ignore malformed descriptors
      const auto& desc = s.as_map();
      StreamConfig sc;
      sc.stream_id = static_cast<std::uint16_t>(find_int(desc, 0).value_or(0));
      sc.width = static_cast<std::uint64_t>(find_pair_elem(desc, 1, 0).value_or(0));
      sc.height = static_cast<std::uint64_t>(find_pair_elem(desc, 1, 1).value_or(0));
      sc.refresh = static_cast<std::uint64_t>(find_int(desc, 2).value_or(0));
      sc.bitrate_kbps = static_cast<std::uint64_t>(find_int(desc, 3).value_or(0));
      c.extra_streams.push_back(sc);
    }
  }
  return c;
}

}  // namespace

Session::Session(HelloParams params) : params_(std::move(params)) {}

void Session::push(Action a) { out_.push_back(std::move(a)); }

std::vector<Action> Session::poll() {
  std::vector<Action> drained;
  drained.swap(out_);
  return drained;
}

void Session::on_event(Event ev) {
  if (state_ == State::Closed || state_ == State::Failed) return;

  switch (ev) {
    case Event::Connected: {
      // Send HELLO (MUST be the first message on the control stream, §3.4).
      Value::Array codecs;
      for (auto c : params_.codecs) codecs.push_back(Value::integer(static_cast<std::int64_t>(c)));
      Value::Map hello;
      hello.emplace_back(Value::integer(0), Value::integer(1));  // protocol_version
      hello.emplace_back(Value::integer(1), Value::text(params_.client_name));
      hello.emplace_back(Value::integer(2), Value::array(std::move(codecs)));
      hello.emplace_back(Value::integer(3), Value::array({Value::integer(params_.max_width),
                                                          Value::integer(params_.max_height)}));
      hello.emplace_back(Value::integer(4), Value::integer(params_.max_refresh));
      hello.emplace_back(Value::integer(5),
                         Value::integer(static_cast<std::int64_t>(params_.features)));
      push({Action::Kind::SendControl, control::encode_frame(control::kHello, hello), 0});
      state_ = State::Negotiating;
      step_ = Step::Welcome;
      break;
    }
    case Event::ConnectionLost:
      state_ = State::Failed;
      push({Action::Kind::Fatal, {}, errors::kInternal});
      break;
    case Event::UserBye:
      push({Action::Kind::SendControl,
            control::encode_frame(control::kBye, {{Value::integer(0), Value::integer(0)}}), 0});
      state_ = State::Closed;
      push({Action::Kind::Closed, {}, 0});
      break;
  }
}

void Session::on_control_bytes(std::span<const std::uint8_t> bytes, std::int64_t now_us) {
  if (state_ == State::Closed || state_ == State::Failed) return;
  rx_.insert(rx_.end(), bytes.begin(), bytes.end());

  std::size_t off = 0;
  while (rx_.size() - off >= 4) {
    const std::uint32_t len = (static_cast<std::uint32_t>(rx_[off]) << 24) |
                              (static_cast<std::uint32_t>(rx_[off + 1]) << 16) |
                              (static_cast<std::uint32_t>(rx_[off + 2]) << 8) |
                              static_cast<std::uint32_t>(rx_[off + 3]);
    if (len > control::kMaxFrameBody) {
      fatal(errors::kProtocolViolation);
      rx_.clear();
      return;
    }
    if (rx_.size() - off < 4 + static_cast<std::size_t>(len)) break;  // frame incomplete
    handle_frame(std::span<const std::uint8_t>(rx_.data() + off, 4 + len), now_us);
    off += 4 + len;
    if (state_ == State::Closed || state_ == State::Failed) {
      rx_.clear();
      return;
    }
  }
  rx_.erase(rx_.begin(), rx_.begin() + static_cast<std::ptrdiff_t>(off));
}

void Session::on_tick(std::int64_t now_us) {
  // Ping every 500 ms once the control stream is usable (§3.8), before START too.
  if (state_ == State::Connecting || state_ == State::Closed || state_ == State::Failed) {
    return;
  }
  if (pinged_ && now_us - last_ping_us_ < 500'000) {
    return;
  }
  pinged_ = true;
  last_ping_us_ = now_us;
  Value::Map body;
  body.emplace_back(Value::integer(0), Value::integer(now_us));
  push({Action::Kind::SendControl, control::encode_frame(control::kClockPing, body), 0});
}

std::vector<std::uint8_t> Session::encode_stats(const StatsInput& in) const {
  Value::Map body;
  body.emplace_back(Value::integer(0),
                    Value::integer(static_cast<std::int64_t>(in.frames_received)));
  body.emplace_back(Value::integer(1),
                    Value::integer(static_cast<std::int64_t>(in.frames_dropped)));
  body.emplace_back(Value::integer(2), Value::integer(static_cast<std::int64_t>(in.datagrams)));
  body.emplace_back(Value::integer(3), Value::floating(in.jitter_ms));
  body.emplace_back(Value::integer(4), Value::integer(static_cast<std::int64_t>(in.decode_us)));
  body.emplace_back(Value::integer(5), Value::integer(static_cast<std::int64_t>(in.rtt_us)));
  if (in.e2e_us) {
    body.emplace_back(Value::integer(6), Value::integer(static_cast<std::int64_t>(*in.e2e_us)));
  }
  // Key 7 (§3.7, multi-display): the stream these counters describe. Omitted for
  // the primary so a single-stream STATS is byte-identical to a pre-feature peer.
  if (in.stream_id != 0) {
    body.emplace_back(Value::integer(7), Value::integer(in.stream_id));
  }
  return control::encode_frame(control::kStats, body);
}

bool Session::send_viewport(std::uint32_t width, std::uint32_t height, std::int64_t now_us) {
  // VIEWPORT is a streaming-phase request (§3.10); ignore it before START.
  if (state_ != State::Streaming) return false;
  if (viewport_sent_ && now_us - last_viewport_us_ < 250'000) return false;

  viewport_sent_ = true;
  last_viewport_us_ = now_us;

  Value::Map body;
  body.emplace_back(Value::integer(0),
                    Value::array({Value::integer(width), Value::integer(height)}));
  push({Action::Kind::SendControl, control::encode_frame(control::kViewport, body), 0});
  return true;
}

void Session::send_config_ack(std::uint64_t generation) {
  // CONFIG_ACK {0: generation} — the host MUST NOT send media for a generation
  // before its ACK (§3.4).
  push({Action::Kind::SendControl,
        control::encode_frame(
            control::kConfigAck,
            {{Value::integer(0), Value::integer(static_cast<std::int64_t>(generation))}}),
        0});
}

void Session::handle_frame(std::span<const std::uint8_t> frame, std::int64_t now_us) {
  auto r = control::decode_frame(frame);
  if (!r) {
    fatal(errors::kProtocolViolation);
    return;
  }
  const control::Decoded& d = r.value();
  if (d.kind == control::Decoded::Kind::Ignored) return;  // unknown msg_type (§3.2)

  const std::uint64_t t = d.msg_type;
  const Value::Map& body = d.body;

  // BYE / ERROR are valid in any phase (§3.9).
  if (t == control::kBye) {
    state_ = State::Closed;
    push({Action::Kind::Closed, {}, 0});
    return;
  }
  if (t == control::kError) {
    const std::uint64_t code =
        static_cast<std::uint64_t>(find_int(body, 0).value_or(errors::kInternal));
    state_ = State::Failed;
    push({Action::Kind::Fatal, {}, code});
    return;
  }
  // CLOCK_PONG is valid in any phase (§7): close the sample with t3 = now.
  if (t == control::kClockPong) {
    auto t0 = find_int(body, 0);
    auto t1 = find_int(body, 1);
    auto t2 = find_int(body, 2);
    if (t0 && t1 && t2) {
      clock_estimate_ = clock_filter_.push(*t0, *t1, *t2, now_us);
    }
    return;
  }

  // Mid-session reconfiguration (§8): a new CONFIG (incremented generation) may
  // arrive while STREAMING — e.g. after a VIEWPORT resolution request. Apply it,
  // ACK it, and ask the app to reinitialize the decoder on the new parameter
  // sets (which ride the next IDR). `frame_seq` continues; media is not torn down.
  if (t == control::kConfig && state_ == State::Streaming) {
    config_ = parse_config(body);
    send_config_ack(config_->generation);
    push({Action::Kind::ConfigChanged, {}, 0});
    return;
  }

  // The remaining setup messages are only meaningful while negotiating.
  if (state_ != State::Negotiating) return;

  switch (step_) {
    case Step::Welcome:
      if (t != control::kWelcome || find_int(body, 0) != 1) {
        fatal(errors::kProtocolViolation);
        return;
      }
      host_name_ = find_text(body, 1);
      // Key 3 (optional): the features the host activated this session (§3.4).
      // Absent ⇒ 0 (no optional features). The app tests kFeatureMultiDisplay.
      active_features_ = static_cast<std::uint64_t>(find_int(body, 3).value_or(0));
      push({Action::Kind::Established, {}, 0});
      step_ = Step::Config;
      return;

    case Step::Config: {
      if (t != control::kConfig) {
        fatal(errors::kProtocolViolation);
        return;
      }
      config_ = parse_config(body);
      send_config_ack(config_->generation);
      step_ = Step::Start;
      return;
    }

    case Step::Start:
      if (t != control::kStart) {
        fatal(errors::kProtocolViolation);
        return;
      }
      state_ = State::Streaming;
      push({Action::Kind::MediaExpected, {}, 0});
      return;
  }
}

void Session::fatal(std::uint64_t code) {
  state_ = State::Failed;
  push({Action::Kind::Fatal, {}, code});
}

}  // namespace loom::core
