#include "loom/core/session.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <span>
#include <vector>

#include "loom/proto/clocksync.hpp"
#include "loom/proto/control.hpp"
#include "loom/proto/errors.hpp"

using loom::core::Action;
using loom::core::Event;
using loom::core::HelloParams;
using loom::core::Session;
using loom::core::State;
using loom::proto::cbor::Value;
namespace control = loom::proto::control;
namespace errors = loom::proto::errors;

namespace {

// Encode a host->client control frame.
std::vector<std::uint8_t> frame(std::uint64_t type, Value::Map body) {
  return control::encode_frame(type, body);
}
std::vector<std::uint8_t> welcome() {
  return frame(control::kWelcome,
               {{Value::integer(0), Value::integer(1)},
                {Value::integer(1), Value::text("test-host")},
                {Value::integer(2), Value::bytes(std::vector<std::uint8_t>(16))}});
}
std::vector<std::uint8_t> config(std::int64_t gen = 1) {
  return frame(control::kConfig,
               {{Value::integer(0), Value::integer(gen)},
                {Value::integer(1), Value::integer(1)},  // HEVC
                {Value::integer(2), Value::array({Value::integer(2560), Value::integer(1440)})},
                {Value::integer(3), Value::integer(72)},
                {Value::integer(4), Value::integer(0)},
                {Value::integer(5), Value::integer(60000)}});
}
std::vector<std::uint8_t> start() { return frame(control::kStart, {}); }

void feed(Session& s, const std::vector<std::uint8_t>& bytes) {
  s.on_control_bytes(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
}

// Find the first SendControl action and decode the frame it carries.
control::Decoded sent(const std::vector<Action>& acts) {
  for (const auto& a : acts) {
    if (a.kind == Action::Kind::SendControl) {
      auto r = control::decode_frame(a.bytes);
      REQUIRE(r.has_value());
      return r.value();
    }
  }
  FAIL("no SendControl action");
  return {};
}
bool has_kind(const std::vector<Action>& acts, Action::Kind k) {
  for (const auto& a : acts)
    if (a.kind == k) return true;
  return false;
}
std::optional<std::uint64_t> fatal_code(const std::vector<Action>& acts) {
  for (const auto& a : acts)
    if (a.kind == Action::Kind::Fatal) return a.code;
  return std::nullopt;
}
std::optional<std::int64_t> body_int(const control::Decoded& d, std::int64_t key) {
  for (const auto& [k, v] : d.body)
    if (k == Value::integer(key) && v.type() == Value::Type::Int) return v.as_int();
  return std::nullopt;
}

}  // namespace

TEST_CASE("connected sends HELLO first") {
  Session s{HelloParams{}};
  s.on_event(Event::Connected);
  const auto acts = s.poll();
  CHECK(s.state() == State::Negotiating);
  CHECK(sent(acts).msg_type == control::kHello);
  CHECK(body_int(sent(acts), 0) == 1);  // protocol_version
}

TEST_CASE("full mirror handshake reaches Streaming") {
  Session s{HelloParams{}};
  s.on_event(Event::Connected);
  s.poll();

  feed(s, welcome());
  auto acts = s.poll();
  CHECK(has_kind(acts, Action::Kind::Established));
  REQUIRE(s.host_name().has_value());
  CHECK(*s.host_name() == "test-host");

  feed(s, config());
  acts = s.poll();
  CHECK(sent(acts).msg_type == control::kConfigAck);
  CHECK(body_int(sent(acts), 0) == 1);  // acked generation
  REQUIRE(s.config().has_value());
  CHECK(s.config()->width == 2560);
  CHECK(s.config()->codec == 1);

  feed(s, start());
  acts = s.poll();
  CHECK(has_kind(acts, Action::Kind::MediaExpected));
  CHECK(s.state() == State::Streaming);
}

TEST_CASE("two frames delivered in one buffer are both processed") {
  Session s{HelloParams{}};
  s.on_event(Event::Connected);
  s.poll();

  std::vector<std::uint8_t> both = welcome();
  const auto c = config();
  both.insert(both.end(), c.begin(), c.end());
  feed(s, both);
  const auto acts = s.poll();
  CHECK(has_kind(acts, Action::Kind::Established));
  CHECK(sent(acts).msg_type == control::kConfigAck);
}

TEST_CASE("a frame split across two reads is reassembled") {
  Session s{HelloParams{}};
  s.on_event(Event::Connected);
  s.poll();

  const auto w = welcome();
  const std::size_t cut = w.size() / 2;
  s.on_control_bytes(std::span<const std::uint8_t>(w.data(), cut));
  CHECK(s.poll().empty());  // nothing actionable yet
  s.on_control_bytes(std::span<const std::uint8_t>(w.data() + cut, w.size() - cut));
  CHECK(has_kind(s.poll(), Action::Kind::Established));
}

TEST_CASE("out-of-order START before WELCOME is a protocol violation") {
  Session s{HelloParams{}};
  s.on_event(Event::Connected);
  s.poll();

  feed(s, start());
  CHECK(fatal_code(s.poll()) == errors::kProtocolViolation);
  CHECK(s.state() == State::Failed);
}

TEST_CASE("CONFIG before WELCOME is a protocol violation") {
  Session s{HelloParams{}};
  s.on_event(Event::Connected);
  s.poll();

  feed(s, config());
  CHECK(fatal_code(s.poll()) == errors::kProtocolViolation);
}

TEST_CASE("ERROR during negotiation is surfaced with its code") {
  Session s{HelloParams{}};
  s.on_event(Event::Connected);
  s.poll();

  feed(s, frame(control::kError, {{Value::integer(0), Value::integer(errors::kBusy)},
                                  {Value::integer(1), Value::text("busy")}}));
  CHECK(fatal_code(s.poll()) == errors::kBusy);
  CHECK(s.state() == State::Failed);
}

TEST_CASE("BYE closes cleanly") {
  Session s{HelloParams{}};
  s.on_event(Event::Connected);
  s.poll();
  feed(s, welcome());
  s.poll();

  feed(s, frame(control::kBye, {{Value::integer(0), Value::integer(1)}}));
  const auto acts = s.poll();
  CHECK(has_kind(acts, Action::Kind::Closed));
  CHECK(s.state() == State::Closed);
}

TEST_CASE("unknown msg_type is ignored, not fatal") {
  Session s{HelloParams{}};
  s.on_event(Event::Connected);
  s.poll();
  feed(s, frame(0x7f, {{Value::integer(0), Value::integer(9)}}));
  const auto acts = s.poll();
  CHECK(acts.empty());
  CHECK(s.state() == State::Negotiating);
}

TEST_CASE("bytes after a terminal state are dropped") {
  Session s{HelloParams{}};
  s.on_event(Event::Connected);
  s.poll();
  feed(s, start());  // -> Failed
  s.poll();
  feed(s, welcome());
  CHECK(s.poll().empty());
}

TEST_CASE("on_tick emits a CLOCK_PING every 500 ms") {
  Session s{HelloParams{}};
  s.on_event(Event::Connected);
  s.poll();

  s.on_tick(0);
  CHECK(sent(s.poll()).msg_type == control::kClockPing);
  s.on_tick(100'000);  // < 500 ms → no ping
  CHECK(s.poll().empty());
  s.on_tick(500'000);  // ≥ 500 ms → ping
  CHECK(sent(s.poll()).msg_type == control::kClockPing);
}

TEST_CASE("encode_stats builds a §3.7 STATS frame") {
  Session s{HelloParams{}};
  loom::core::StatsInput in;
  in.frames_received = 10;
  in.rtt_us = 4000;
  in.e2e_us = 32000;
  const auto d = control::decode_frame(s.encode_stats(in));
  REQUIRE(d.has_value());
  CHECK(d.value().msg_type == control::kStats);
  CHECK(body_int(d.value(), 0) == 10);
  CHECK(body_int(d.value(), 5) == 4000);
  CHECK(body_int(d.value(), 6) == 32000);  // e2e present
}

// Drive a session all the way to STREAMING, returning it ready for streaming-
// phase tests.
Session streaming_session() {
  Session s{HelloParams{}};
  s.on_event(Event::Connected);
  s.poll();
  feed(s, welcome());
  s.poll();
  feed(s, config());
  s.poll();
  feed(s, start());
  s.poll();
  REQUIRE(s.state() == State::Streaming);
  return s;
}

TEST_CASE("mid-session CONFIG is applied, ACKed, and raises ConfigChanged") {
  Session s = streaming_session();

  // A new generation with a smaller resolution (a VIEWPORT-driven reconfig, §8).
  feed(s, frame(control::kConfig,
                {{Value::integer(0), Value::integer(2)},
                 {Value::integer(1), Value::integer(1)},
                 {Value::integer(2), Value::array({Value::integer(1920), Value::integer(1080)})},
                 {Value::integer(3), Value::integer(72)},
                 {Value::integer(4), Value::integer(0)},
                 {Value::integer(5), Value::integer(40000)}}));
  const auto acts = s.poll();

  CHECK(s.state() == State::Streaming);  // stays streaming; media is not torn down
  CHECK(has_kind(acts, Action::Kind::ConfigChanged));
  CHECK(sent(acts).msg_type == control::kConfigAck);
  CHECK(body_int(sent(acts), 0) == 2);  // ACKs the new generation
  REQUIRE(s.config().has_value());
  CHECK(s.config()->generation == 2);
  CHECK(s.config()->width == 1920);
  CHECK(s.config()->height == 1080);
}

TEST_CASE("send_viewport emits VIEWPORT and is rate-limited to 1 per 250 ms") {
  Session s = streaming_session();

  CHECK(s.send_viewport(2560, 1440, 1'000'000));
  auto d = sent(s.poll());
  CHECK(d.msg_type == control::kViewport);
  {
    // Body {0: [w, h]}.
    bool found = false;
    for (const auto& [k, v] : d.body) {
      if (k == Value::integer(0) && v.type() == Value::Type::Array) {
        REQUIRE(v.as_array().size() == 2);
        CHECK(v.as_array()[0].as_int() == 2560);
        CHECK(v.as_array()[1].as_int() == 1440);
        found = true;
      }
    }
    CHECK(found);
  }

  // Within 250 ms: suppressed, no action queued.
  CHECK_FALSE(s.send_viewport(1920, 1080, 1'100'000));
  CHECK(s.poll().empty());

  // After 250 ms: allowed again.
  CHECK(s.send_viewport(1920, 1080, 1'300'000));
  CHECK(sent(s.poll()).msg_type == control::kViewport);
}

TEST_CASE("send_viewport is ignored before STREAMING") {
  Session s{HelloParams{}};
  s.on_event(Event::Connected);
  s.poll();
  CHECK_FALSE(s.send_viewport(2560, 1440, 1'000'000));
  CHECK(s.poll().empty());
}

TEST_CASE("clock sync: CLOCK_PONGs through the session match the min-filter") {
  // The min-filter is vector-proven in clocksync_test; this proves the *live
  // wire path* (encode/decode PONG → filter) preserves the samples end to end.
  Session s{HelloParams{}};
  s.on_event(Event::Connected);
  s.poll();

  struct Sample {
    std::int64_t t0, t1, t2, t3;
  };
  const std::vector<Sample> samples = {
      {1000, 1500, 1600, 2200},
      {2000, 2400, 2450, 2900},
      {3000, 3600, 3650, 4500},
      {4000, 4200, 4260, 4700},
  };
  loom::proto::clocksync::ClockFilter reference;
  loom::proto::clocksync::Estimate expected{};
  for (const auto& smp : samples) {
    const auto pong = frame(control::kClockPong, {{Value::integer(0), Value::integer(smp.t0)},
                                                  {Value::integer(1), Value::integer(smp.t1)},
                                                  {Value::integer(2), Value::integer(smp.t2)}});
    s.on_control_bytes(std::span<const std::uint8_t>(pong.data(), pong.size()), smp.t3);
    expected = reference.push(smp.t0, smp.t1, smp.t2, smp.t3);
  }
  REQUIRE(s.clock().has_value());
  CHECK(s.clock()->rtt == expected.rtt);
  CHECK(s.clock()->offset == expected.offset);
}
