#include "video_pipeline.hpp"

#include <cstdio>
#include <utility>

namespace loom::sdl {

VideoPipeline::VideoPipeline(IdrRequestFn on_idr) : receiver_(std::move(on_idr)) {
  thread_ = std::thread([this] { decode_loop(); });
}

VideoPipeline::~VideoPipeline() {
  receiver_.stop();  // wakes a blocked pop_au() so the decode thread can exit
  if (thread_.joinable()) {
    thread_.join();
  }
}

std::int64_t VideoPipeline::now_ms() const {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                               start_)
      .count();
}

void VideoPipeline::feed_datagram(std::span<const std::uint8_t> datagram) {
  receiver_.feed_datagram(datagram, now_ms());
}

std::shared_ptr<const DecodedFrame> VideoPipeline::take_frame() {
  std::lock_guard<std::mutex> lk(frame_mu_);
  if (!has_new_) {
    return nullptr;
  }
  has_new_ = false;
  return latest_;
}

void VideoPipeline::decode_loop() {
  HevcDecoder decoder;
  while (auto au = receiver_.pop_au()) {
    DecodedFrame frame;
    if (decoder.decode(au->data, frame)) {
      frame.capture_ts = au->capture_ts;
      auto shared = std::make_shared<const DecodedFrame>(std::move(frame));
      {
        std::lock_guard<std::mutex> lk(frame_mu_);
        latest_ = shared;
        has_new_ = true;
      }
      std::fprintf(stderr, "{\"event\":\"frame_decoded\",\"w\":%d,\"h\":%d}\n", shared->width,
                   shared->height);
    }
  }
}

}  // namespace loom::sdl
