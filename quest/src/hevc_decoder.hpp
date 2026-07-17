#pragma once
// AMediaCodec HEVC decoder in surface mode, on its own thread.
//
// Surface mode: the codec renders decoded frames straight into the SurfaceTexture
// window — there is no CPU-side frame and no copy. The render thread pulls the
// newest frame with SurfaceTexture::update(); this class never hands frames to
// anyone, it just keeps the codec fed and drained.
//
// It is fed by a loom::core::VideoReceiver: the decode thread blocks on
// pop_au() and feeds each access unit to the codec. The access unit's host
// capture_ts (§4.1) is passed through as the codec presentation timestamp, so it
// rides back out on the SurfaceTexture frame timestamp for the e2e latency
// overlay (§4.5).

#include <media/NdkMediaCodec.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "loom/core/video_receiver.hpp"

struct ANativeWindow;

namespace loom::quest {

// Decode-latency instrumentation (input-queue → output-dequeue per frame) and
// the deepest the decoder ever buffered. The R5 verdict lives in
// reviews/M3.2-mediacodec-r5.md; kept live for the overlay and smoothness work.
struct DecodeMetrics {
  std::vector<float> latencies_ms;
  float latest_ms = 0.0f;  // most recent decode latency, for the live overlay
  int max_in_flight = 0;
  bool low_latency_requested = false;
};

class HevcDecoder {
 public:
  HevcDecoder() = default;
  ~HevcDecoder();

  HevcDecoder(const HevcDecoder&) = delete;
  HevcDecoder& operator=(const HevcDecoder&) = delete;

  // Configures an HEVC decoder rendering into `surface`. Does not start.
  bool create(ANativeWindow* surface, int width, int height);

  // Launches the decode thread, pulling access units from `receiver`. The
  // receiver must outlive the decoder; stop() unblocks it.
  void start(loom::core::VideoReceiver* receiver);
  void stop();

  const DecodeMetrics& metrics() const { return metrics_; }

 private:
  void decode_loop();

  AMediaCodec* codec_ = nullptr;
  loom::core::VideoReceiver* receiver_ = nullptr;

  std::thread thread_;
  std::atomic<bool> running_{false};

  DecodeMetrics metrics_;
};

}  // namespace loom::quest
