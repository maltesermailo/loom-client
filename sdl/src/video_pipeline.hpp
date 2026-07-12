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
    std::uint64_t frames_dropped = 0;   // reassembler drops (loss + gap + stale)
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
  void deliver_frame(std::uint32_t frame_seq);
  void prune(std::uint32_t newest);

  Counters counters_;

  IdrRequestFn on_idr_;
  loom::proto::reassembly::Reassembler reasm_;
  std::size_t seen_events_ = 0;

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
