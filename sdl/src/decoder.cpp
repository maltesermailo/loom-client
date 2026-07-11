#include "decoder.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
}

namespace loom::sdl {

HevcDecoder::HevcDecoder() {
  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
  if (codec == nullptr) {
    return;
  }
  ctx_ = avcodec_alloc_context3(codec);
  if (ctx_ == nullptr) {
    return;
  }
  // Low delay, no frame threading → each access unit yields its frame at once.
  ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
  ctx_->thread_count = 1;
  if (avcodec_open2(ctx_, codec, nullptr) < 0) {
    avcodec_free_context(&ctx_);
    return;
  }
  pkt_ = av_packet_alloc();
  frame_ = av_frame_alloc();
}

HevcDecoder::~HevcDecoder() {
  if (frame_ != nullptr) av_frame_free(&frame_);
  if (pkt_ != nullptr) av_packet_free(&pkt_);
  if (ctx_ != nullptr) avcodec_free_context(&ctx_);
}

bool HevcDecoder::decode(std::span<const std::uint8_t> au, DecodedFrame& out) {
  if (ctx_ == nullptr) {
    return false;
  }
  pkt_->data = const_cast<std::uint8_t*>(au.data());
  pkt_->size = static_cast<int>(au.size());
  if (avcodec_send_packet(ctx_, pkt_) < 0) {
    return false;
  }
  if (avcodec_receive_frame(ctx_, frame_) < 0) {
    return false;  // EAGAIN (needs more input) or error
  }
  if (frame_->format != AV_PIX_FMT_YUV420P) {
    return false;  // §5 mandates 4:2:0 8-bit; anything else is a host bug
  }

  const int w = frame_->width;
  const int h = frame_->height;
  const int cw = w / 2;
  const int ch = h / 2;
  out.width = w;
  out.height = h;
  out.y.resize(static_cast<std::size_t>(w) * h);
  out.u.resize(static_cast<std::size_t>(cw) * ch);
  out.v.resize(static_cast<std::size_t>(cw) * ch);

  // Copy plane-by-plane, dropping libav's row padding (linesize ≥ width).
  for (int row = 0; row < h; ++row) {
    std::copy_n(frame_->data[0] + static_cast<std::ptrdiff_t>(row) * frame_->linesize[0], w,
                out.y.begin() + static_cast<std::ptrdiff_t>(row) * w);
  }
  for (int row = 0; row < ch; ++row) {
    std::copy_n(frame_->data[1] + static_cast<std::ptrdiff_t>(row) * frame_->linesize[1], cw,
                out.u.begin() + static_cast<std::ptrdiff_t>(row) * cw);
    std::copy_n(frame_->data[2] + static_cast<std::ptrdiff_t>(row) * frame_->linesize[2], cw,
                out.v.begin() + static_cast<std::ptrdiff_t>(row) * cw);
  }
  return true;
}

}  // namespace loom::sdl
