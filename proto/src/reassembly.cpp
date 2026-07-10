#include "loom/proto/reassembly.hpp"

#include <algorithm>

namespace loom::proto::reassembly {

void Reassembler::push(std::int64_t t_ms, const Fragment& frag) {
  const std::uint32_t seq = frag.frame_seq;

  // Rule 1: fragments at or below the newest completed frame are stale.
  if (static_cast<std::int64_t>(seq) <= newest_complete_) {
    counters_.stale_fragments++;
    return;
  }

  if (incomplete_.find(seq) == incomplete_.end()) {
    // Rule 2: hold at most two in-flight incomplete frames.
    if (incomplete_.size() >= 2) {
      const std::uint32_t oldest = incomplete_.begin()->first; // ordered map: min key
      if (seq > oldest) {
        incomplete_.erase(oldest);
        counters_.dropped_incomplete++;
      } else {
        // Older than everything currently in the window: treat as stale.
        counters_.stale_fragments++;
        return;
      }
    }
    incomplete_.emplace(seq, Incomplete{frag.frag_count, {}, frag.keyframe});
  }

  Incomplete& entry = incomplete_.at(seq);
  entry.have.insert(frag.frag_index); // duplicates are idempotent
  if (entry.have.size() != entry.need) return;

  // Frame complete.
  const bool keyframe = entry.keyframe;
  incomplete_.erase(seq);
  newest_complete_ = std::max(newest_complete_, static_cast<std::int64_t>(seq));

  // Rule-1 cleanup: any still-incomplete frame now at/below newest dies.
  for (auto it = incomplete_.begin(); it != incomplete_.end();) {
    if (static_cast<std::int64_t>(it->first) <= newest_complete_) {
      it = incomplete_.erase(it);
      counters_.dropped_incomplete++;
    } else {
      ++it;
    }
  }

  // Rule 3: decode gating.
  if (keyframe) {
    deliver(t_ms, seq, true);
  } else if (last_decoded_ && *last_decoded_ == static_cast<std::int64_t>(seq) - 1) {
    deliver(t_ms, seq, false);
  } else {
    counters_.discarded_gap++;
    maybe_idr(t_ms);
  }
}

void Reassembler::deliver(std::int64_t t_ms, std::uint32_t seq, bool keyframe) {
  events_.push_back(Event{Event::Kind::Deliver, t_ms, seq, keyframe, 0});
  last_decoded_ = static_cast<std::int64_t>(seq);
  // An outstanding IDR clears once we deliver a keyframe newer than the
  // last-good frame recorded when the request was raised (§3.6).
  if (keyframe && idr_outstanding_) {
    if (!idr_last_good_at_request_ || static_cast<std::int64_t>(seq) > *idr_last_good_at_request_)
      idr_outstanding_ = false;
  }
}

void Reassembler::maybe_idr(std::int64_t t_ms) {
  if (idr_outstanding_) return;
  if (idr_last_t_ && t_ms - *idr_last_t_ < 250) return; // rate limit: >= 250 ms apart
  const std::int64_t last_good = last_decoded_.value_or(0);
  events_.push_back(
      Event{Event::Kind::IdrRequest, t_ms, 0, false, static_cast<std::uint32_t>(last_good)});
  idr_outstanding_ = true;
  idr_last_t_ = t_ms;
  idr_last_good_at_request_ = last_decoded_;
}

} // namespace loom::proto::reassembly
