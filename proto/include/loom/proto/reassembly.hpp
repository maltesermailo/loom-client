#pragma once
// Client receive model — fragment reassembly, decode gating, and IDR-request
// logic (PROTOCOL.md §6 rules 1-3 + §3.6).
//
// Driven by (t_ms, Fragment) tuples exactly as the reassembly trace vectors are,
// with no clock of its own, so it is fully deterministic and replayable.
//
// State invariants:
//   * newest_complete_ = highest frame_seq ever fully reassembled (-1 = none).
//     A fragment with frame_seq <= newest_complete_ is stale (rule 1).
//   * incomplete_ holds at most 2 in-flight frames (rule 2), keyed by frame_seq
//     and ordered, so begin() is the oldest. have is a set, so duplicate
//     fragments are idempotent.
//   * last_decoded_ = newest frame handed to the decoder. A completed frame is
//     delivered iff it is a keyframe or frame_seq == last_decoded_ + 1 (rule 3);
//     otherwise it is discarded and the IDR logic runs.
//   * At most one IDR request is outstanding; requests are >= 250 ms apart. An
//     outstanding request clears when a keyframe newer than the last-good frame
//     recorded at request time is delivered (§3.6).
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace loom::proto::reassembly {

// The video-datagram header fields the receive model needs, plus arrival time.
struct Fragment {
  std::uint32_t frame_seq;
  std::uint16_t frag_index;
  std::uint16_t frag_count;
  bool keyframe;
};

struct Event {
  enum class Kind { Deliver, IdrRequest };
  Kind kind;
  std::int64_t t_ms;
  std::uint32_t frame_seq = 0;  // Deliver only
  bool keyframe = false;        // Deliver only
  std::uint32_t last_good = 0;  // IdrRequest only (newest fully-decoded frame, 0 if none)
  bool operator==(const Event&) const = default;
};

struct Counters {
  std::uint64_t dropped_incomplete = 0;  // rule-2 evictions + rule-1 cleanup of incompletes
  std::uint64_t discarded_gap = 0;       // completed frames failing decode gating (§6.3)
  std::uint64_t stale_fragments = 0;     // rule-1 stale + below-window drops
  bool operator==(const Counters&) const = default;
};

class Reassembler {
 public:
  // Feed one fragment that arrived at t_ms; appends any resulting events.
  void push(std::int64_t t_ms, const Fragment& frag);
  const std::vector<Event>& events() const { return events_; }
  const Counters& counters() const { return counters_; }

 private:
  struct Incomplete {
    std::uint16_t need;
    std::set<std::uint16_t> have;
    bool keyframe;
  };
  void deliver(std::int64_t t_ms, std::uint32_t seq, bool keyframe);
  void maybe_idr(std::int64_t t_ms);

  std::int64_t newest_complete_ = -1;
  std::optional<std::int64_t> last_decoded_;
  std::map<std::uint32_t, Incomplete> incomplete_;
  std::vector<Event> events_;
  Counters counters_;
  bool idr_outstanding_ = false;
  std::optional<std::int64_t> idr_last_t_;
  std::optional<std::int64_t> idr_last_good_at_request_;
};

}  // namespace loom::proto::reassembly
