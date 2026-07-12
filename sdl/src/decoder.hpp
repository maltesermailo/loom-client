#pragma once
// HevcDecoder — libavcodec HEVC decode (M1.2). Feeds complete Annex-B access
// units in, yields decoded I420 frames. Low-delay, single-threaded (no frame
// reordering) to match the §5 chain and keep latency tight. Runs on the video
// pipeline's dedicated decode thread.

#include <cstdint>
#include <span>
#include <vector>

struct AVCodecContext;
struct AVPacket;
struct AVFrame;

namespace loom::sdl {

// A self-contained decoded I420 frame (owns its pixels; safe to hand across
// threads). Planes are tightly packed: y_stride = width, uv_stride = width/2.
struct DecodedFrame {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> y;
  std::vector<std::uint8_t> u;
  std::vector<std::uint8_t> v;
  std::uint64_t capture_ts = 0;  // host-clock µs (§4.1), set by the pipeline
  std::uint64_t decode_us = 0;   // decode duration, set by the decoder
};

class HevcDecoder {
public:
  HevcDecoder();
  ~HevcDecoder();
  HevcDecoder(const HevcDecoder&) = delete;
  HevcDecoder& operator=(const HevcDecoder&) = delete;

  bool ok() const { return ctx_ != nullptr; }

  // Decode one Annex-B access unit. Returns true and fills `out` if a frame was
  // produced (1:1 in low-delay mode).
  bool decode(std::span<const std::uint8_t> au, DecodedFrame& out);

private:
  AVCodecContext* ctx_ = nullptr;
  AVPacket* pkt_ = nullptr;
  AVFrame* frame_ = nullptr;
};

}  // namespace loom::sdl
