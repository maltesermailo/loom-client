#include "au_splitter.hpp"

namespace loom::quest {
namespace {

// HEVC NAL unit types (H.265 Table 7-1). VCL types are 0..31; the IRAP range
// (BLA/IDR/CRA) is 16..23, and any of those makes an access unit a keyframe.
constexpr std::uint8_t kVclMax = 31;
constexpr std::uint8_t kIrapMin = 16;
constexpr std::uint8_t kIrapMax = 23;

std::uint8_t nal_type(std::uint8_t first_byte) { return (first_byte >> 1) & 0x3F; }

// Offset of the next Annex-B start code (00 00 01 or 00 00 00 01) at or after
// `from`, or stream.size() if none remains.
std::size_t next_start_code(const std::vector<std::uint8_t>& s, std::size_t from) {
  for (std::size_t i = from; i + 3 <= s.size(); ++i) {
    if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1) return i;
  }
  return s.size();
}

}  // namespace

std::vector<AccessUnit> split_access_units(const std::vector<std::uint8_t>& stream) {
  std::vector<AccessUnit> units;

  std::size_t pos = next_start_code(stream, 0);
  AccessUnit current;
  bool current_has_vcl = false;

  while (pos < stream.size()) {
    // This NAL runs from its start code up to the next one.
    const std::size_t next = next_start_code(stream, pos + 3);

    // Skip the start code to read the NAL header byte: 2-byte codes are always
    // preceded by a leading zero, so the header sits at pos+3 either way.
    const std::size_t header = pos + 3;
    const std::uint8_t type = header < stream.size() ? nal_type(stream[header]) : 0xFF;
    const bool is_vcl = type <= kVclMax;

    // A VCL NAL after we already have one in hand opens a new picture: flush the
    // current access unit first (its parameter sets / SEI already accumulated).
    if (is_vcl && current_has_vcl) {
      units.push_back(std::move(current));
      current = AccessUnit{};
      current_has_vcl = false;
    }

    current.data.insert(current.data.end(), stream.begin() + pos, stream.begin() + next);
    if (is_vcl) {
      current_has_vcl = true;
      if (type >= kIrapMin && type <= kIrapMax) current.keyframe = true;
    }

    pos = next;
  }

  if (current_has_vcl) units.push_back(std::move(current));

  return units;
}

}  // namespace loom::quest
