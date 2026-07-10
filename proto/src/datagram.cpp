#include "loom/proto/datagram.hpp"

namespace loom::proto {

DatagramHeader make_header(bool keyframe, std::uint16_t stream_id, std::uint32_t frame_seq,
                           std::uint16_t frag_index, std::uint16_t frag_count) {
  const bool last = frag_count > 0 && frag_index == static_cast<std::uint16_t>(frag_count - 1);
  return DatagramHeader{keyframe, last, stream_id, frame_seq, frag_index, frag_count};
}

std::uint8_t flags_byte(const DatagramHeader& h) {
  std::uint8_t f = 0;
  if (h.keyframe) f |= kFlagKeyframe;
  if (h.last_fragment) f |= kFlagLastFragment;
  return f;
}

std::array<std::uint8_t, kHeaderLen> encode_header(const DatagramHeader& h) {
  std::array<std::uint8_t, kHeaderLen> b{};
  b[0] = kMagic;
  b[1] = flags_byte(h);
  b[2] = static_cast<std::uint8_t>(h.stream_id >> 8);
  b[3] = static_cast<std::uint8_t>(h.stream_id);
  b[4] = static_cast<std::uint8_t>(h.frame_seq >> 24);
  b[5] = static_cast<std::uint8_t>(h.frame_seq >> 16);
  b[6] = static_cast<std::uint8_t>(h.frame_seq >> 8);
  b[7] = static_cast<std::uint8_t>(h.frame_seq);
  b[8] = static_cast<std::uint8_t>(h.frag_index >> 8);
  b[9] = static_cast<std::uint8_t>(h.frag_index);
  b[10] = static_cast<std::uint8_t>(h.frag_count >> 8);
  b[11] = static_cast<std::uint8_t>(h.frag_count);
  return b;
}

std::vector<std::uint8_t> encode_datagram(const DatagramHeader& h,
                                          std::span<const std::uint8_t> payload) {
  const auto head = encode_header(h);
  std::vector<std::uint8_t> out;
  out.reserve(kHeaderLen + payload.size());
  out.insert(out.end(), head.begin(), head.end());
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

const char* to_string(DropReason r) {
  switch (r) {
  case DropReason::too_short:
    return "too_short";
  case DropReason::oversize:
    return "oversize";
  case DropReason::bad_magic:
    return "bad_magic";
  case DropReason::frag_count_zero:
    return "frag_count_zero";
  case DropReason::frag_index_range:
    return "frag_index_range";
  case DropReason::last_fragment_mismatch:
    return "last_fragment_mismatch";
  case DropReason::unknown_stream:
    return "unknown_stream";
  }
  return "unknown"; // unreachable; silences -Wreturn-type
}

static std::uint16_t be16(std::span<const std::uint8_t> b, std::size_t o) {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(b[o]) << 8) | b[o + 1]);
}

Result<DecodedDatagram, DropReason> decode(std::span<const std::uint8_t> bytes) {
  if (bytes.size() < kHeaderLen) return Err(DropReason::too_short);
  if (bytes.size() > kMaxDatagramLen) return Err(DropReason::oversize);
  if (bytes[0] != kMagic) return Err(DropReason::bad_magic);

  const std::uint8_t flags = bytes[1];
  const std::uint16_t stream_id = be16(bytes, 2);
  const std::uint32_t frame_seq = (static_cast<std::uint32_t>(bytes[4]) << 24) |
                                  (static_cast<std::uint32_t>(bytes[5]) << 16) |
                                  (static_cast<std::uint32_t>(bytes[6]) << 8) |
                                  static_cast<std::uint32_t>(bytes[7]);
  const std::uint16_t frag_index = be16(bytes, 8);
  const std::uint16_t frag_count = be16(bytes, 10);

  if (frag_count < 1) return Err(DropReason::frag_count_zero);
  if (frag_index >= frag_count) return Err(DropReason::frag_index_range);
  const bool last = (flags & kFlagLastFragment) != 0;
  if (last != (frag_index == static_cast<std::uint16_t>(frag_count - 1)))
    return Err(DropReason::last_fragment_mismatch);
  if (stream_id != 0 && stream_id != 1) return Err(DropReason::unknown_stream);

  const DatagramHeader h{(flags & kFlagKeyframe) != 0, last,       stream_id,
                         frame_seq,                    frag_index, frag_count};
  return Ok(DecodedDatagram{h, bytes.size() - kHeaderLen});
}

} // namespace loom::proto
