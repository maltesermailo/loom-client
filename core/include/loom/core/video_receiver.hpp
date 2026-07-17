#pragma once
// VideoReceiver — media datagrams → decodable access units, transport- and
// decoder-agnostic. The shared receive pipeline both clients run: the SDL client
// (libavcodec decode) and the Quest client (AMediaCodec surface decode) feed it
// the same datagrams and pop the same access units; only the decoder differs.
//
// It runs the vector-proven loom::proto reassembly state machine over datagram
// headers to decide delivery/loss (§6), reassembles each delivered frame's
// payload, strips the §4.1 capture_ts, and hands the Annex-B access unit to a
// bounded queue a decode thread drains with pop_au().
//
// Freshness over completeness (ARCHITECTURE §0): the hand-off holds at most two
// access units. Once the decoder is that far behind, whatever waits is stale, so
// it is dropped rather than queued — without the cap, latency climbs for as long
// as decode is slower than arrival. Dropping breaks the reference chain (§5.3),
// so it forces an IDR request and discards frames until the next keyframe. Every
// IDR request — from a lost fragment or from a decoder-overrun drop — funnels
// through one gate so §3.6's "at most one per 250 ms, none while one outstanding"
// holds for the client as a whole.
//
// Threading: feed_datagram() on the producer (transport/render) thread, pop_au()
// on the decode thread. counters() reads producer-thread state and must be
// called from the producer thread.

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <span>
#include <vector>

#include "loom/proto/reassembly.hpp"

namespace loom::core {

// One decodable frame: its host capture timestamp (§4.1) and one Annex-B HEVC
// access unit (start codes included).
struct AccessUnit {
  std::uint64_t capture_ts = 0;
  std::vector<std::uint8_t> data;
  bool keyframe = false;
};

class VideoReceiver {
 public:
  // Invoked from the producer thread when a lost fragment or a decoder-overrun
  // drop forces an IDR request; the argument is last_good_frame_seq (§3.6).
  using IdrRequestFn = std::function<void(std::uint32_t)>;

  explicit VideoReceiver(IdrRequestFn on_idr);
  ~VideoReceiver();
  VideoReceiver(const VideoReceiver&) = delete;
  VideoReceiver& operator=(const VideoReceiver&) = delete;

  struct Counters {
    std::uint64_t frames_received = 0;  // access units handed to the decoder
    std::uint64_t frames_dropped = 0;   // reassembler drops + decoder-overrun drops
    std::uint64_t datagrams = 0;        // video datagrams accepted
    std::uint64_t bytes = 0;            // video datagram bytes (for bitrate)
  };

  // Feed one media datagram. `now_ms` is a monotonic clock for reassembly timing
  // and the §3.6 IDR rate limit. Producer thread.
  void feed_datagram(std::span<const std::uint8_t> datagram, std::int64_t now_ms);

  // Block until an access unit is ready, then return it; returns nullopt once
  // stop() has been called and the queue is drained. Decode thread.
  std::optional<AccessUnit> pop_au();

  // Wake a blocked pop_au() for shutdown.
  void stop();

  // Producer-thread snapshot.
  Counters counters() const { return counters_; }

 private:
  void deliver_frame(std::uint32_t frame_seq, bool keyframe, std::int64_t now_ms);
  void request_idr(std::uint32_t last_good, std::int64_t now_ms);
  void prune(std::uint32_t newest);

  IdrRequestFn on_idr_;
  Counters counters_;

  loom::proto::reassembly::Reassembler reasm_;
  std::size_t seen_events_ = 0;

  // Producer-thread only.
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

  // The bounded decode hand-off (producer → decode thread).
  std::mutex mu_;
  std::condition_variable cv_;
  std::queue<AccessUnit> au_queue_;
  bool stop_ = false;
};

}  // namespace loom::core
