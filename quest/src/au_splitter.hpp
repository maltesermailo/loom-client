#pragma once
// Splits a raw Annex-B HEVC byte stream into access units — one per frame.
//
// Only needed for the M3.2 offline path: the looped .hevc file arrives as one
// undivided byte blob, but the decoder wants one access unit per queued input
// buffer so each frame gets a distinct presentation timestamp (which R5's
// latency measurement keys on). In M3.3 the transport delivers frame-sized
// access units directly and this is not used.

#include <cstdint>
#include <vector>

namespace loom::quest {

struct AccessUnit {
  std::vector<std::uint8_t> data;  // Annex-B, start codes included
  bool keyframe = false;
};

// Parses the whole stream up front. Relies on the §5.5 one-slice-per-frame
// guarantee: each VCL NAL is a complete picture, so an access unit ends at its
// single VCL NAL and any parameter sets / SEI that follow belong to the next.
std::vector<AccessUnit> split_access_units(const std::vector<std::uint8_t>& stream);

}  // namespace loom::quest
