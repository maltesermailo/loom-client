#pragma once
// AMediaCodec HEVC decoder in surface mode, on its own thread.
//
// Surface mode: the codec renders decoded frames straight into the SurfaceTexture
// window — there is no CPU-side frame and no copy. The render thread pulls the
// newest frame with SurfaceTexture::update(); this class never hands frames to
// anyone, it just keeps the codec fed and drained.
//
// M3.2 feeds it a fixed, looped access-unit list (no network). The threading and
// the feed/drain loop are the shape M3.3 keeps — only the access-unit source
// changes, from this looped list to core's reassembly output.

#include <media/NdkMediaCodec.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "au_splitter.hpp"

struct ANativeWindow;

namespace loom::quest {

// R5 (ARCHITECTURE §13): is low-latency mode actually honored? Measured, not
// trusted. Latencies are input-queue → output-dequeue per frame; max_in_flight
// is the deepest the decoder ever buffered (the "no more than 1 queued" check).
struct DecodeMetrics {
  std::vector<float> latencies_ms;
  int max_in_flight = 0;
  bool low_latency_requested = false;
};

class HevcDecoder {
 public:
  HevcDecoder() = default;
  ~HevcDecoder();

  HevcDecoder(const HevcDecoder&) = delete;
  HevcDecoder& operator=(const HevcDecoder&) = delete;

  // Configures an HEVC decoder rendering into `surface`. `access_units` is the
  // looped source; it is copied. Does not start the thread.
  bool create(ANativeWindow* surface, int width, int height, std::vector<AccessUnit> access_units);

  void start();
  void stop();

  // Snapshot of the R5 measurement so far. Thread-safe to read after stop().
  const DecodeMetrics& metrics() const { return metrics_; }

 private:
  void decode_loop();

  AMediaCodec* codec_ = nullptr;
  std::vector<AccessUnit> access_units_;
  int width_ = 0;
  int height_ = 0;

  std::thread thread_;
  std::atomic<bool> running_{false};

  DecodeMetrics metrics_;
};

}  // namespace loom::quest
