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

// Short polling timeout keeps the decode thread alternating feed/drain without
// spinning; ~0.35 ms of latency granularity (measured in R5).
constexpr int64_t kDequeueTimeoutUs = 2000;

// Gather this many samples before emitting the decode-latency summary once.
constexpr std::size_t kMetricsSample = 1000;

// Cap on frames queued to the codec but not yet output. Freshness over
// completeness (§6.2/§5.6): bursty WiFi arrival would otherwise let the decode
// thread stuff several frames into the codec at once, and each extra frame is a
// frame-time of pure latency. Holding at ~2 keeps the codec near real-time; when
// arrival outruns it, frames back up in the receiver instead, whose own cap then
// drops-stale and requests an IDR.
constexpr std::size_t kMaxInFlight = 2;

int64_t monotonic_ns() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

void log_decode_summary(const DecodeMetrics& m) {
  std::vector<float> sorted = m.latencies_ms;
  std::sort(sorted.begin(), sorted.end());

  const auto pct = [&sorted](float p) {
    return sorted[static_cast<std::size_t>(p * (sorted.size() - 1))];
  };
  float sum = 0.0f;
  for (float v : sorted) sum += v;

  LOOM_LOGI("decode latency (ms): n=%zu min=%.2f p50=%.2f p95=%.2f max=%.2f mean=%.2f",
            sorted.size(), sorted.front(), pct(0.50f), pct(0.95f), sorted.back(),
            sum / sorted.size());
  LOOM_LOGI("decode max frames in flight: %d", m.max_in_flight);
}

}  // namespace

HevcDecoder::~HevcDecoder() {
  stop();
  if (codec_ != nullptr) AMediaCodec_delete(codec_);
}

bool HevcDecoder::create(ANativeWindow* surface, int width, int height) {
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
  // c2.qti.hevc.decoder — the §5 bitstream (no B-frames, single reference) has
  // nothing to reorder, so the decoder already emits each frame the moment it is
  // decoded (1 frame in flight with or without the flag). Kept as correct,
  // portable intent. See reviews/M3.2-mediacodec-r5.md.
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

void HevcDecoder::start(loom::core::VideoReceiver* receiver) {
  receiver_ = receiver;
  if (AMediaCodec_start(codec_) != AMEDIA_OK) {
    LOOM_LOGE("AMediaCodec_start failed");
    return;
  }
  running_ = true;
  thread_ = std::thread([this] { decode_loop(); });
}

void HevcDecoder::stop() {
  running_ = false;
  if (receiver_ != nullptr) receiver_->stop();  // unblocks a waiting pop_au()
  if (thread_.joinable()) thread_.join();
  if (codec_ != nullptr) AMediaCodec_stop(codec_);
}

void HevcDecoder::decode_loop() {
  // Wall-clock time each pts was queued, so a dequeued output becomes an
  // input→output latency. Small: only in-flight frames live here.
  std::unordered_map<int64_t, int64_t> queued_ns;

  // Drain ready outputs; render=true sends each to the SurfaceTexture. The first
  // dequeue blocks up to `first_timeout_us` (so a gated loop waits for a slot
  // instead of spinning); the rest are non-blocking.
  const auto drain_outputs = [&](int64_t first_timeout_us) {
    int64_t timeout = first_timeout_us;
    for (;;) {
      AMediaCodecBufferInfo info;
      const ssize_t out_idx = AMediaCodec_dequeueOutputBuffer(codec_, &info, timeout);
      timeout = 0;
      if (out_idx < 0) break;

      const auto it = queued_ns.find(info.presentationTimeUs);
      if (it != queued_ns.end()) {
        const float latency_ms = (monotonic_ns() - it->second) / 1.0e6f;
        queued_ns.erase(it);
        metrics_.latest_ms = latency_ms;
        if (metrics_.latencies_ms.size() < kMetricsSample) {
          metrics_.latencies_ms.push_back(latency_ms);
          if (metrics_.latencies_ms.size() == kMetricsSample) log_decode_summary(metrics_);
        }
      }
      AMediaCodec_releaseOutputBuffer(codec_, out_idx, true);
    }
  };

  while (running_) {
    // Hold the codec at the freshness cap: if it already has kMaxInFlight frames
    // undecoded, block for one to come out rather than piling on more latency.
    // Otherwise drain non-blocking, presenting finished frames and freeing input
    // buffers so the codec does not run ahead of us.
    const bool gated = queued_ns.size() >= kMaxInFlight;
    drain_outputs(gated ? kDequeueTimeoutUs : 0);
    if (gated) continue;

    // Reserve an input buffer BEFORE consuming an access unit. Popping first and
    // then finding no buffer would strand the access unit — dropping it silently
    // breaks the reference chain (§5.3) with no IDR request. If the codec has no
    // free slot yet, loop back to drain and retry instead.
    const ssize_t in_idx = AMediaCodec_dequeueInputBuffer(codec_, kDequeueTimeoutUs);
    if (in_idx < 0) continue;

    auto au = receiver_->pop_au();  // blocks for the next frame
    if (!au) break;                 // receiver stopped

    size_t capacity = 0;
    uint8_t* buffer = AMediaCodec_getInputBuffer(codec_, in_idx, &capacity);
    if (buffer == nullptr || au->data.size() > capacity) continue;
    std::memcpy(buffer, au->data.data(), au->data.size());

    // Pass capture_ts through as the codec pts: it rides back out on the
    // SurfaceTexture frame timestamp for the e2e overlay (§4.5).
    const int64_t pts = static_cast<int64_t>(au->capture_ts);
    queued_ns[pts] = monotonic_ns();
    AMediaCodec_queueInputBuffer(codec_, in_idx, 0, au->data.size(), static_cast<uint64_t>(pts), 0);

    const int in_flight = static_cast<int>(queued_ns.size());
    if (in_flight > metrics_.max_in_flight) metrics_.max_in_flight = in_flight;
  }
}

}  // namespace loom::quest
