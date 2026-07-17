#include "hevc_decoder.hpp"

#include <android/native_window.h>
#include <media/NdkMediaFormat.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <unordered_map>

#include "log.hpp"

namespace loom::quest {
namespace {

constexpr const char* kHevcMime = "video/hevc";

// 72 Hz frame interval. The input is paced to this so the measured latency is a
// real per-frame input→output figure, not a throughput artifact of dumping the
// whole stream at the codec at once.
constexpr int64_t kFrameIntervalUs = 1000000 / 72;

// Short polling timeouts keep the single decode thread alternating between
// feeding and draining without spinning.
constexpr int64_t kDequeueTimeoutUs = 2000;

int64_t monotonic_ns() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

// Skip this many decoded frames before recording, so codec warm-up and the
// first file-loop boundary don't pollute the steady-state R5 numbers.
constexpr int64_t kWarmupFrames = 150;

// Number of steady-state latency samples to gather before the R5 verdict line.
constexpr std::size_t kMetricsSample = 1000;

// Logs the R5 distribution. Runs on the decode thread, which owns metrics_, so
// no locking is needed. ARCHITECTURE §13: measure whether low-latency is honored.
void log_r5_summary(const DecodeMetrics& m) {
  std::vector<float> sorted = m.latencies_ms;
  std::sort(sorted.begin(), sorted.end());

  const auto pct = [&sorted](float p) {
    return sorted[static_cast<std::size_t>(p * (sorted.size() - 1))];
  };
  float sum = 0.0f;
  for (float v : sorted) sum += v;

  LOOM_LOGI("R5 decode latency (ms): n=%zu min=%.2f p50=%.2f p95=%.2f p99=%.2f max=%.2f mean=%.2f",
            sorted.size(), sorted.front(), pct(0.50f), pct(0.95f), pct(0.99f), sorted.back(),
            sum / sorted.size());
  LOOM_LOGI("R5 max frames in flight: %d (low_latency_requested=%d)", m.max_in_flight,
            m.low_latency_requested);
}

}  // namespace

HevcDecoder::~HevcDecoder() {
  stop();
  if (codec_ != nullptr) AMediaCodec_delete(codec_);
}

bool HevcDecoder::create(ANativeWindow* surface, int width, int height,
                         std::vector<AccessUnit> access_units) {
  width_ = width;
  height_ = height;
  access_units_ = std::move(access_units);

  codec_ = AMediaCodec_createDecoderByType(kHevcMime);
  if (codec_ == nullptr) {
    LOOM_LOGE("createDecoderByType(%s) failed", kHevcMime);
    return false;
  }

  AMediaFormat* format = AMediaFormat_new();
  AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, kHevcMime);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, width);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, height);

  // Request low-latency decoding. R5 measured this to be a no-op on the Quest's
  // c2.qti.hevc.decoder — but only because the §5 bitstream (no B-frames, single
  // reference) has nothing to reorder, so the decoder already emits each frame
  // the moment it is decoded: 1 frame in flight with or without the flag. We keep
  // the standard key as correct, portable intent; the QTI vendor key was dropped
  // after measuring it changed nothing. See reviews/M3.2 for the numbers.
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_LOW_LATENCY, 1);
  metrics_.low_latency_requested = true;

  const media_status_t status = AMediaCodec_configure(codec_, format, surface, nullptr, 0);
  AMediaFormat_delete(format);
  if (status != AMEDIA_OK) {
    LOOM_LOGE("AMediaCodec_configure failed: %d", status);
    return false;
  }

  return true;
}

void HevcDecoder::start() {
  if (AMediaCodec_start(codec_) != AMEDIA_OK) {
    LOOM_LOGE("AMediaCodec_start failed");
    return;
  }
  running_ = true;
  thread_ = std::thread([this] { decode_loop(); });
}

void HevcDecoder::stop() {
  running_ = false;
  if (thread_.joinable()) thread_.join();
  if (codec_ != nullptr) AMediaCodec_stop(codec_);
}

void HevcDecoder::decode_loop() {
  // Wall-clock time each pts was queued, so a dequeued output can be turned into
  // an input→output latency. Small: only in-flight frames live here.
  std::unordered_map<int64_t, int64_t> queued_ns;

  std::size_t au_index = 0;
  int64_t input_index = 0;
  int64_t output_count = 0;
  int64_t next_feed_ns = monotonic_ns();

  while (running_) {
    // --- Feed one access unit, paced to 72 Hz. ---
    const int64_t now = monotonic_ns();
    if (now >= next_feed_ns) {
      const ssize_t in_idx = AMediaCodec_dequeueInputBuffer(codec_, kDequeueTimeoutUs);
      if (in_idx >= 0) {
        const AccessUnit& au = access_units_[au_index];
        size_t capacity = 0;
        uint8_t* buffer = AMediaCodec_getInputBuffer(codec_, in_idx, &capacity);
        if (buffer != nullptr && au.data.size() <= capacity) {
          std::memcpy(buffer, au.data.data(), au.data.size());

          const int64_t pts = input_index * kFrameIntervalUs;
          queued_ns[pts] = monotonic_ns();
          AMediaCodec_queueInputBuffer(codec_, in_idx, 0, au.data.size(),
                                       static_cast<uint64_t>(pts), 0);

          ++input_index;
          au_index = (au_index + 1) % access_units_.size();  // loop → IDR restart

          // Pace to 72 Hz, but never burst to "catch up" after a stall: if we
          // fell more than a frame behind (codec init, a loop-boundary IDR), snap
          // the schedule to now instead of feeding a backlog all at once. Bursting
          // was building a permanent queue that dwarfed the codec's real latency.
          next_feed_ns += kFrameIntervalUs * 1000;
          const int64_t feed_now = monotonic_ns();
          if (next_feed_ns < feed_now) next_feed_ns = feed_now + kFrameIntervalUs * 1000;

          // Steady-state depth only (warm-up inflates it as the codec fills).
          if (output_count >= kWarmupFrames) {
            const int in_flight = static_cast<int>(queued_ns.size());
            if (in_flight > metrics_.max_in_flight) metrics_.max_in_flight = in_flight;
          }
        }
      }
    }

    // --- Drain ALL ready output; render=true sends each to the SurfaceTexture.
    // Draining to empty each pass ensures a decoded frame never waits on us, so
    // the measured latency is the codec's own, not a scheduling artifact. ---
    for (;;) {
      AMediaCodecBufferInfo info;
      const ssize_t out_idx = AMediaCodec_dequeueOutputBuffer(codec_, &info, kDequeueTimeoutUs);
      if (out_idx < 0) break;

      const auto it = queued_ns.find(info.presentationTimeUs);
      if (it != queued_ns.end()) {
        const float latency_ms = (monotonic_ns() - it->second) / 1.0e6f;
        queued_ns.erase(it);

        if (output_count >= kWarmupFrames && metrics_.latencies_ms.size() < kMetricsSample) {
          metrics_.latencies_ms.push_back(latency_ms);
          if (metrics_.latencies_ms.size() == kMetricsSample) log_r5_summary(metrics_);
        }
        ++output_count;
      }
      AMediaCodec_releaseOutputBuffer(codec_, out_idx, true);
    }
  }
}

}  // namespace loom::quest
