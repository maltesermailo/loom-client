#pragma once
// VideoPipeline — datagrams → reassembly → HEVC decode, off the main thread.
//
// The main loop feeds raw media datagrams in (feed_datagram). The pipeline runs
// the vector-proven loom::proto reassembly state machine over their headers to
// decide delivery/loss (§6), reassembles the payload bytes of each delivered
// frame, strips the §4.1 capture_ts, and queues the Annex-B access unit to a
// dedicated decode thread. take_frame() hands the newest decoded frame to the
// renderer. On a reassembly IDR request it invokes the caller's callback (which
// sends IDR_REQUEST on the control stream).
//
// The decode-thread hand-off holds at most two access units. Once the decoder is
// that far behind, whatever is waiting is already stale, and
// "a late frame is worth less than a dropped one" (ARCHITECTURE §0) — so it is
// dropped rather than queued. Without that cap the queue grows without bound
// whenever decode is slower than arrival, and end-to-end latency climbs for as
// long as the session runs (measured: ~4 s within 20 s at 1440p72, where
// software decode sustains ~59 fps against 72 fps of arrivals).
//
// Dropping breaks the reference chain — every non-IDR frame references its
// predecessor (§5.3) — so a drop forces an IDR request and everything up to the
// next keyframe is discarded rather than fed to the decoder as garbage. All IDR
// requests, from here and from the reassembler, funnel through request_idr() so
// §3.6's "at most one per 250 ms, none while one is outstanding" holds for the
// client as a whole; the reassembler enforces that rule only for its own.

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <span>
#include <thread>
#include <vector>

#include "decoder.hpp"
#include "loom/proto/reassembly.hpp"

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

  // Cumulative receive counters (main-thread reads).
  struct Counters {
    std::uint64_t frames_received = 0;  // frames delivered to the decoder
    std::uint64_t frames_dropped = 0;   // reassembler drops + stale-at-the-decoder drops
    std::uint64_t datagrams = 0;        // video datagrams accepted
    std::uint64_t bytes = 0;            // video datagram bytes (for bitrate)
  };

  // Feed one media datagram (main thread).
  void feed_datagram(std::span<const std::uint8_t> datagram);

  // Newest decoded frame since the last call, or nullptr (main thread).
  std::shared_ptr<const DecodedFrame> take_frame();

  // Snapshot of the cumulative counters (main thread).
  Counters counters() const { return counters_; }

 private:
  void decode_loop();
  std::int64_t now_ms() const;
  void deliver_frame(std::uint32_t frame_seq, bool keyframe);
  void request_idr(std::uint32_t last_good);
  void prune(std::uint32_t newest);

  Counters counters_;

  IdrRequestFn on_idr_;
  loom::proto::reassembly::Reassembler reasm_;
  std::size_t seen_events_ = 0;

  // Feed-thread only. Frames dropped because the decoder was still busy, plus
  // the §3.6 request gate covering every IDR request this client sends.
  std::uint64_t stale_at_decoder_ = 0;
  bool awaiting_keyframe_ = false;
  bool idr_outstanding_ = false;
  std::int64_t last_idr_request_ms_ = -1;
  std::uint32_t last_good_seq_ = 0;

  // Fragment payloads awaiting assembly, keyed by frame_seq.
  struct Pending {
    std::uint16_t count = 0;
    std::map<std::uint16_t, std::vector<std::uint8_t>> parts;
  };
  std::map<std::uint32_t, Pending> pending_;
  std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();

  // One access unit queued for decode, with its host capture timestamp.
  struct QueuedAu {
    std::uint64_t capture_ts;
    std::vector<std::uint8_t> data;
  };

  // Decode thread + its work queue and latest-frame slot.
  std::thread thread_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::queue<QueuedAu> au_queue_;
  std::shared_ptr<const DecodedFrame> latest_;
  bool has_new_ = false;
  bool stop_ = false;
};

}  // namespace loom::sdl
