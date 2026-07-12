#include "loom/proto/datagram.hpp"

#include <doctest/doctest.h>

#include <string>
#include <string_view>
#include <vector>

#include "hex_util.hpp"

using loom::proto::decode;
using loom::proto::DropReason;
using loom::proto::encode_datagram;
using loom::proto::make_header;
using loomtest::from_hex;
using loomtest::to_hex;

namespace {
// Encode a datagram to hex. Empty payload yields the 12-byte header alone.
std::string enc(bool kf, std::uint16_t sid, std::uint32_t seq, std::uint16_t idx, std::uint16_t cnt,
                std::string_view payload_hex) {
  return to_hex(encode_datagram(make_header(kf, sid, seq, idx, cnt), from_hex(payload_hex)));
}
}  // namespace

TEST_CASE("encode derives KEYFRAME/LAST flags and big-endian layout") {
  // keyframe + single fragment (idx0/cnt1 => last) => flags 0x03
  CHECK(enc(true, 0, 0, 0, 1, "aaaaaaaa") == "4c0300000000000000000001aaaaaaaa");
  // keyframe + first of three (not last) => flags 0x01
  CHECK(enc(true, 0, 0, 0, 3, "") == "4c0100000000000000000003");
  // keyframe + last of three => flags 0x03
  CHECK(enc(true, 0, 0, 2, 3, "") == "4c0300000000000000020003");
  // audio, not keyframe, single fragment => LAST only => flags 0x02
  CHECK(enc(false, 1, 7, 0, 1, "") == "4c0200010000000700000001");
  // max frame_seq, empty payload
  CHECK(enc(false, 0, 0xFFFFFFFF, 0, 1, "") == "4c020000ffffffff00000001");
}

TEST_CASE("decode accepts a valid header and reports payload length") {
  const auto b = from_hex("4c03000000000005000000010102");
  const auto r = decode(b);
  REQUIRE(r.has_value());
  const auto& d = r.value();
  CHECK(d.header.keyframe);
  CHECK(d.header.last_fragment);
  CHECK(d.header.stream_id == 0);
  CHECK(d.header.frame_seq == 5);
  CHECK(d.header.frag_index == 0);
  CHECK(d.header.frag_count == 1);
  CHECK(d.payload_len == 2);
}

TEST_CASE("decode enforces every validation rule, in order (with reason strings)") {
  struct Case {
    const char* hex;
    DropReason reason;
    const char* str;
  };
  const Case cases[] = {
      {"4c0300", DropReason::too_short, "too_short"},
      {"4d0200000000000000000001", DropReason::bad_magic, "bad_magic"},
      {"4c0200000000000000000000", DropReason::frag_count_zero, "frag_count_zero"},
      {"4c0200000000000000030003", DropReason::frag_index_range, "frag_index_range"},
      {"4c0000000000000000020003", DropReason::last_fragment_mismatch, "last_fragment_mismatch"},
      {"4c0200000000000000000003", DropReason::last_fragment_mismatch, "last_fragment_mismatch"},
      {"4c0200090000000000000001", DropReason::unknown_stream, "unknown_stream"},
  };
  for (const auto& c : cases) {
    const auto r = decode(from_hex(c.hex));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() == c.reason);
    CHECK(std::string(to_string(r.error())) == c.str);
  }
}

TEST_CASE("decode oversize vs the 1350-byte boundary") {
  const std::vector<std::uint8_t> over_payload(1339, 0x00);  // 12 + 1339 = 1351
  const auto over = encode_datagram(make_header(false, 0, 0, 0, 1), over_payload);
  CHECK(over.size() == 1351);
  REQUIRE_FALSE(decode(over).has_value());
  CHECK(decode(over).error() == DropReason::oversize);
  CHECK(std::string(to_string(DropReason::oversize)) == "oversize");

  const std::vector<std::uint8_t> max_payload(1338, 0x00);  // 12 + 1338 = 1350 (allowed)
  const auto ok = encode_datagram(make_header(false, 0, 0, 0, 1), max_payload);
  CHECK(ok.size() == 1350);
  CHECK(decode(ok).has_value());
}

TEST_CASE("decode ignores reserved flag bits") {
  // flags 0xff = KEYFRAME|LAST|all reserved bits; reserved bits ignored.
  const auto b = from_hex("4cff00000000000100000001ee");
  const auto r = decode(b);
  REQUIRE(r.has_value());
  CHECK(r.value().header.keyframe);
  CHECK(r.value().header.last_fragment);
  CHECK(r.value().payload_len == 1);
}
