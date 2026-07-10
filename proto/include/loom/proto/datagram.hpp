#pragma once
// Video/audio datagram framing — PROTOCOL.md §4. 12-byte big-endian header:
// magic(1) | flags(1) | stream_id(2) | frame_seq(4) | frag_index(2) |
// frag_count(2). flags bit0=KEYFRAME, bit1=LAST_FRAGMENT, bits2-7 reserved.
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "loom/proto/result.hpp"

namespace loom::proto {

inline constexpr std::uint8_t kMagic = 0x4C; // 'L'
inline constexpr std::uint8_t kFlagKeyframe = 0x01;
inline constexpr std::uint8_t kFlagLastFragment = 0x02;
inline constexpr std::size_t kHeaderLen = 12;
inline constexpr std::size_t kMaxDatagramLen = 1350;

struct DatagramHeader {
  bool keyframe;
  bool last_fragment;
  std::uint16_t stream_id;
  std::uint32_t frame_seq;
  std::uint16_t frag_index;
  std::uint16_t frag_count;
};

// Build a header, deriving last_fragment from position (§4: LAST set iff last).
DatagramHeader make_header(bool keyframe, std::uint16_t stream_id, std::uint32_t frame_seq,
                           std::uint16_t frag_index, std::uint16_t frag_count);

// The flags byte for a header (reserved bits always zero).
std::uint8_t flags_byte(const DatagramHeader& h);

// Serialize the 12-byte big-endian header.
std::array<std::uint8_t, kHeaderLen> encode_header(const DatagramHeader& h);

// Serialize header followed by payload into one datagram.
std::vector<std::uint8_t> encode_datagram(const DatagramHeader& h,
                                          std::span<const std::uint8_t> payload);

// Why a datagram was dropped. All are silent drops in production (§6.6); the
// strings exist only for the conformance vectors.
enum class DropReason {
  too_short,
  oversize,
  bad_magic,
  frag_count_zero,
  frag_index_range,
  last_fragment_mismatch,
  unknown_stream,
};

// The stable reason string used by the conformance vectors.
const char* to_string(DropReason r);

struct DecodedDatagram {
  DatagramHeader header;
  std::size_t payload_len;
};

// Decode and validate a datagram header per §4. Validation order is normative.
Result<DecodedDatagram, DropReason> decode(std::span<const std::uint8_t> bytes);

} // namespace loom::proto
