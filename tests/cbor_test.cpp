#include <doctest/doctest.h>

#include <string>

#include "hex_util.hpp"
#include "loom/proto/cbor.hpp"

using loom::proto::cbor::decode;
using loom::proto::cbor::encode_canonical;
using loom::proto::cbor::Value;
using loomtest::from_hex;
using loomtest::to_hex;

static std::string enc(const Value& v) { return to_hex(encode_canonical(v)); }

TEST_CASE("canonical integers use shortest form") {
  CHECK(enc(Value::integer(0)) == "00");
  CHECK(enc(Value::integer(23)) == "17");
  CHECK(enc(Value::integer(24)) == "1818");
  CHECK(enc(Value::integer(90)) == "185a");
  CHECK(enc(Value::integer(256)) == "190100");
  CHECK(enc(Value::integer(60000)) == "19ea60");
  CHECK(enc(Value::integer(1000000)) == "1a000f4240");
  CHECK(enc(Value::integer(-1)) == "20");
  CHECK(enc(Value::integer(-240)) == "38ef");
}

TEST_CASE("canonical strings, bytes, and simple values") {
  CHECK(enc(Value::text("Quest 3")) == "6751756573742033");
  CHECK(enc(Value::text("")) == "60");
  CHECK(enc(Value::bytes({0x00, 0x01, 0x02})) == "43000102");
  CHECK(enc(Value::boolean(true)) == "f5");
  CHECK(enc(Value::boolean(false)) == "f4");
  CHECK(enc(Value::null()) == "f6");
}

TEST_CASE("canonical maps sort by bytewise-encoded key") {
  Value::Map m; // inserted out of order
  m.emplace_back(Value::integer(2), Value::integer(0));
  m.emplace_back(Value::integer(0), Value::integer(0));
  m.emplace_back(Value::integer(1), Value::integer(0));
  CHECK(enc(Value::map(std::move(m))) == "a3000001000200");
  // a large key (0x18 0x63) sorts after single-byte keys
  Value::Map m2;
  m2.emplace_back(Value::integer(99), Value::integer(1));
  m2.emplace_back(Value::integer(1), Value::integer(2));
  CHECK(enc(Value::map(std::move(m2))) == "a20102186301");
}

TEST_CASE("nested arrays encode in order") {
  Value::Array inner{Value::integer(2), Value::integer(3)};
  Value::Array a{Value::integer(1), Value::array(std::move(inner))};
  CHECK(enc(Value::array(std::move(a))) == "8201820203");
}

TEST_CASE("decode round-trips and tolerates non-canonical input") {
  Value::Map m;
  m.emplace_back(Value::integer(0), Value::integer(1));
  m.emplace_back(Value::integer(1), Value::text("hi"));
  m.emplace_back(Value::integer(2), Value::bytes({0xde, 0xad}));
  const Value v = Value::map(std::move(m));
  const auto bytes = encode_canonical(v);
  REQUIRE(decode(bytes).has_value());
  CHECK(*decode(bytes) == v);
  // 1 encoded the long way (0x1b + 8 bytes) still decodes to integer(1).
  const auto nc = from_hex("1b0000000000000001");
  REQUIRE(decode(nc).has_value());
  CHECK(*decode(nc) == Value::integer(1));
}

TEST_CASE("canonical floats use the shortest lossless form") {
  CHECK(enc(Value::floating(2.5)) == "f94100");  // exact in f16
  CHECK(enc(Value::floating(0.0)) == "f90000");
  CHECK(enc(Value::floating(1.0)) == "f93c00");
  CHECK(enc(Value::floating(0.1)) == "fb3fb999999999999a"); // not f16/f32-exact -> f64
  REQUIRE(decode(from_hex("f94100")).has_value());
  CHECK(*decode(from_hex("f94100")) == Value::floating(2.5));
  REQUIRE(decode(from_hex("fb3fb999999999999a")).has_value());
  CHECK(*decode(from_hex("fb3fb999999999999a")) == Value::floating(0.1));
}

TEST_CASE("decode rejects truncated and indefinite-length input") {
  CHECK_FALSE(decode(from_hex("82")).has_value());   // array(2) with no items
  CHECK_FALSE(decode(from_hex("8205")).has_value());  // array(2) with one item
  CHECK_FALSE(decode(from_hex("9f")).has_value());    // indefinite-length array
}
