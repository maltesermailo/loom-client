#pragma once
// VideoPipeline — the SDL client's decode half over the shared receive pipeline.
//
// The transport-agnostic receive logic (reassembly, access-unit assembly, the
// §3.6 IDR policy, and the freshness-capped hand-off) lives in
// loom::core::VideoReceiver, shared verbatim with the Quest client. This class
// is only what is SDL-specific: a decode thread that pops access units from the
// receiver, runs them through libavcodec, and publishes the newest frame for the
// renderer. On a lost fragment the receiver invokes the caller's IDR callback.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <thread>

#include "decoder.hpp"
#include "loom/core/video_receiver.hpp"

namespace loom::sdl {

class VideoPipeline {
 public:
  // Called (on the feeding thread) when a lost fragment forces an IDR request;
  // the argument is last_good_frame_seq (§3.6).
  using IdrRequestFn = std::function<void(std::uint32_t)>;

  explicit VideoPipeline(IdrRequestFn on_idr);
  ~VideoPipeline();
  VideoPipeline(const VideoPipeline&) = delete;
  VideoPipeline& operator=(const VideoPipeline&) = delete;

  using Counters = loom::core::VideoReceiver::Counters;

  // Feed one media datagram (main thread).
  void feed_datagram(std::span<const std::uint8_t> datagram);

  // Newest decoded frame since the last call, or nullptr (main thread).
  std::shared_ptr<const DecodedFrame> take_frame();

  // Snapshot of the cumulative counters (main thread).
  Counters counters() const { return receiver_.counters(); }

 private:
  void decode_loop();
  std::int64_t now_ms() const;

  loom::core::VideoReceiver receiver_;
  std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();

  // Decode thread + the newest-frame slot it publishes to the renderer.
  std::thread thread_;
  std::mutex frame_mu_;
  std::shared_ptr<const DecodedFrame> latest_;
  bool has_new_ = false;
};

}  // namespace loom::sdl
