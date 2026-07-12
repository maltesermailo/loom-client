#pragma once
// Metrics — single-threaded instrumentation aggregator (main loop).
//
// Folds in one sample per displayed frame (decode time, e2e latency, jitter) and
// derives the §3.7 STATS window + the overlay's current values. Frame/loss/
// datagram counts and bitrate come from the VideoPipeline's cumulative counters;
// rtt/offset come from core::Session's clock. This struct only does arithmetic —
// no threads, no I/O.

#include <cstdint>

#include "loom/core/session.hpp"
#include "video_pipeline.hpp"

namespace loom::sdl {

// Snapshot for the on-screen overlay.
struct OverlayStats {
  std::uint64_t e2e_us = 0;
  std::uint64_t decode_us = 0;
  std::uint64_t rtt_us = 0;
  std::uint64_t bitrate_kbps = 0;
  double loss_pct = 0.0;
  bool have_clock = false;
};

class Metrics {
 public:
  // Fold in one displayed frame. `arrival_us`/`e2e_us` are client-clock µs.
  void on_frame(std::uint64_t capture_ts, std::int64_t arrival_us, std::uint64_t decode_us,
                std::uint64_t e2e_us) {
    decode_sum_ += decode_us;
    e2e_sum_ += e2e_us;
    ++n_;
    last_decode_us_ = decode_us;
    last_e2e_us_ = e2e_us;
    // RFC 3550-style jitter; capture/arrival deltas cancel the clock offset.
    if (prev_capture_ >= 0) {
      std::int64_t d =
          (arrival_us - prev_arrival_) - (static_cast<std::int64_t>(capture_ts) - prev_capture_);
      if (d < 0) d = -d;
      jitter_ms_ += (static_cast<double>(d) / 1000.0 - jitter_ms_) / 16.0;
    }
    prev_capture_ = static_cast<std::int64_t>(capture_ts);
    prev_arrival_ = arrival_us;
  }

  // Build the STATS body input for this window and reset the per-window means.
  loom::core::StatsInput take_window(const VideoPipeline::Counters& cur, std::uint64_t rtt_us,
                                     bool have_clock) {
    loom::core::StatsInput in;
    in.frames_received = cur.frames_received - base_.frames_received;
    in.frames_dropped = cur.frames_dropped - base_.frames_dropped;
    in.datagrams = cur.datagrams - base_.datagrams;
    in.jitter_ms = jitter_ms_;
    in.decode_us = n_ ? decode_sum_ / n_ : 0;
    in.rtt_us = rtt_us;
    if (have_clock && n_ > 0) {
      in.e2e_us = e2e_sum_ / n_;
    }
    base_ = cur;
    decode_sum_ = 0;
    e2e_sum_ = 0;
    n_ = 0;
    return in;
  }

  // Current values for the overlay (does not reset the window).
  OverlayStats overlay(const VideoPipeline::Counters& cur, std::uint64_t rtt_us, bool have_clock,
                       std::uint64_t bitrate_kbps) const {
    OverlayStats o;
    o.e2e_us = last_e2e_us_;
    o.decode_us = last_decode_us_;
    o.rtt_us = rtt_us;
    o.bitrate_kbps = bitrate_kbps;
    o.have_clock = have_clock;
    const std::uint64_t total = cur.frames_received + cur.frames_dropped;
    o.loss_pct = total ? 100.0 * static_cast<double>(cur.frames_dropped) / total : 0.0;
    return o;
  }

 private:
  std::uint64_t decode_sum_ = 0, e2e_sum_ = 0, n_ = 0;
  std::uint64_t last_decode_us_ = 0, last_e2e_us_ = 0;
  double jitter_ms_ = 0.0;
  std::int64_t prev_capture_ = -1, prev_arrival_ = -1;
  VideoPipeline::Counters base_;
};

}  // namespace loom::sdl
