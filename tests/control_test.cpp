#include <doctest/doctest.h>

#include <initializer_list>
#include <string>
#include <utility>

#include "hex_util.hpp"
#include "loom/proto/control.hpp"

using loom::proto::cbor::Value;
using loom::proto::control::decode_frame;
using loom::proto::control::Decoded;
using loom::proto::control::encode_frame;
using loom::proto::control::to_string;
using loomtest::from_hex;
using loomtest::to_hex;
namespace control = loom::proto::control;

namespace {
Value::Map body(std::initializer_list<std::pair<std::int64_t, Value>> entries) {
  Value::Map m;
  for (const auto& [k, v] : entries) m.emplace_back(Value::integer(k), v);
  return m;
}
bool has_key(const Value::Map& m, std::int64_t k) {
  for (const auto& [key, v] : m)
    if (key == Value::integer(k)) return true;
  return false;
}
} // namespace

TEST_CASE("encode_frame is canonical and length-prefixed (matches vectors)") {
  CHECK(to_hex(encode_frame(control::kConfigAck, body({{0, Value::integer(1)}}))) ==
        "000000058204a10001");
  CHECK(to_hex(encode_frame(control::kStart, {})) == "000000038205a0");
  // Full HELLO must match the conformance vector byte-for-byte.
  const auto hello = body({
      {0, Value::integer(1)},
      {1, Value::text("Quest 3")},
      {2, Value::array({Value::integer(1), Value::integer(2)})},
      {3, Value::array({Value::integer(3072), Value::integer(3216)})},
      {4, Value::integer(90)},
      {5, Value::integer(1)},
  });
  CHECK(to_hex(encode_frame(control::kHello, hello)) ==
        "0000001f8201a60001016751756573742033028201020382190c00190c9004185a0501");
}

TEST_CASE("decode_frame round-trips a registered message") {
  const auto frame = from_hex("000000188203a6000101010282190a001905a003184804010519ea60"); // CONFIG
  const auto r = decode_frame(frame);
  REQUIRE(r.has_value());
  CHECK(r.value().kind == Decoded::Kind::Message);
  CHECK(r.value().msg_type == control::kConfig);
  CHECK(r.value().body.size() == 6);
}

TEST_CASE("decode_frame strips unknown body keys") {
  const auto frame = encode_frame(control::kHello, body({{0, Value::integer(1)},
                                                         {99, Value::text("future")}}));
  const auto r = decode_frame(frame);
  REQUIRE(r.has_value());
  CHECK(r.value().kind == Decoded::Kind::Message);
  CHECK(r.value().body.size() == 1);
  CHECK(has_key(r.value().body, 0));
  CHECK_FALSE(has_key(r.value().body, 99));
}

TEST_CASE("decode_frame ignores unregistered message types") {
  const auto frame = encode_frame(0x7f, body({{0, Value::integer(123)}}));
  const auto r = decode_frame(frame);
  REQUIRE(r.has_value());
  CHECK(r.value().kind == Decoded::Kind::Ignored);
}

TEST_CASE("decode_frame rejects framing and envelope violations") {
  const char* bad[] = {
      "00010001",           // length field alone exceeds 65536
      "00000004a1616101",   // envelope not an array (a map)
      "000000048301a003",   // wrong arity [1, {}, 3]
      "000000058205820102", // body not a map [5, [1, 2]]
      "000000028205",       // truncated CBOR body
  };
  for (const char* h : bad) {
    const auto r = decode_frame(from_hex(h));
    REQUIRE_FALSE(r.has_value());
    CHECK(std::string(to_string(r.error())) == "PROTOCOL_VIOLATION");
  }
}
