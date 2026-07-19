#include "loom/core/video_receiver.hpp"

#include <utility>

#include "loom/proto/datagram.hpp"

namespace loom::core {
namespace {

// §3.6: the client MUST NOT send more than one IDR_REQUEST per 250 ms.
constexpr std::int64_t kIdrMinIntervalMs = 250;

// Access units the decoder may have in hand before we call the oldest stale. Two
// matches the "at most 2 frames in flight" the receive model works in
// (ARCHITECTURE §4.2, PROTOCOL §6.2): one in flight plus one waiting absorbs a
// single slow decode without dropping, while still bounding the hand-off.
constexpr std::size_t kMaxPendingAus = 2;

// The frame body prefix carrying the host capture timestamp (§4.1).
constexpr std::size_t kCaptureTsLen = 8;

// Keep a few frames of payloads around; older incomplete ones are dead (§6.2).
constexpr std::uint32_t kPruneKeep = 4;

namespace reassembly = loom::proto::reassembly;

}  // namespace

VideoReceiver::VideoReceiver(IdrRequestFn on_idr, std::uint16_t stream_id)
    : on_idr_(std::move(on_idr)), stream_id_(stream_id) {}

VideoReceiver::~VideoReceiver() { stop(); }

void VideoReceiver::feed_datagram(std::span<const std::uint8_t> datagram, std::int64_t now_ms) {
  // Accept this receiver's own stream_id (≥ 2 requires the negotiated-streams
  // form of decode; 0/1 are always valid), then keep only our stream.
  const std::uint16_t mine[] = {stream_id_};
  auto decoded = loom::proto::decode_with_streams(datagram, mine);
  if (!decoded) {
    return;  // malformed / un-negotiated stream → silent drop (§6.6)
  }
  const auto& h = decoded.value().header;
  if (h.stream_id != stream_id_) {
    return;  // another stream's datagram (or audio) — not ours
  }
  counters_.datagrams += 1;
  counters_.bytes += datagram.size();

  // Stash this fragment's payload for later assembly.
  auto& pending = pending_[h.frame_seq];
  pending.count = h.frag_count;
  pending.parts[h.frag_index] =
      std::vector<std::uint8_t>(datagram.begin() + loom::proto::kHeaderLen, datagram.end());

  // Feed header metadata to the receive-model state machine.
  reasm_.push(now_ms, reassembly::Fragment{h.frame_seq, h.frag_index, h.frag_count, h.keyframe});

  const auto& events = reasm_.events();
  for (; seen_events_ < events.size(); ++seen_events_) {
    const auto& e = events[seen_events_];
    if (e.kind == reassembly::Event::Kind::Deliver) {
      deliver_frame(e.frame_seq, e.keyframe, now_ms);
    } else if (e.kind == reassembly::Event::Kind::IdrRequest) {
      request_idr(e.last_good, now_ms);
    }
  }
  // frames dropped = every reassembler drop category (loss / gap / stale), plus
  // the frames we dropped at the decoder input for being stale on arrival.
  const auto& c = reasm_.counters();
  counters_.frames_dropped =
      c.dropped_incomplete + c.discarded_gap + c.stale_fragments + stale_at_decoder_;
  prune(h.frame_seq);
}

void VideoReceiver::deliver_frame(std::uint32_t frame_seq, bool keyframe, std::int64_t now_ms) {
  auto it = pending_.find(frame_seq);
  if (it == pending_.end() || it->second.parts.size() != it->second.count) {
    return;  // shouldn't happen: reassembler only delivers complete frames
  }
  std::vector<std::uint8_t> body;
  for (std::uint16_t i = 0; i < it->second.count; ++i) {
    const auto& part = it->second.parts[i];
    body.insert(body.end(), part.begin(), part.end());
  }
  pending_.erase(it);
  if (body.size() <= kCaptureTsLen) {
    return;
  }
  // Read the capture_ts prefix (§4.1, u64 BE); the rest is one Annex-B AU.
  std::uint64_t capture_ts = 0;
  for (std::size_t i = 0; i < kCaptureTsLen; ++i) {
    capture_ts = (capture_ts << 8) | body[i];
  }
  std::vector<std::uint8_t> au(body.begin() + kCaptureTsLen, body.end());

  // Anything still waiting on the decoder is stale the moment a newer frame is
  // ready: drop it instead of letting the hand-off grow into a latency store.
  bool chain_broken = false;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (au_queue_.size() >= kMaxPendingAus) {
      stale_at_decoder_ += au_queue_.size();
      std::queue<AccessUnit>().swap(au_queue_);
      chain_broken = true;
    }
  }
  if (chain_broken) {
    awaiting_keyframe_ = true;
  }

  // A frame whose reference we just dropped decodes to garbage, so it waits for
  // the IDR instead (§5.3 — every non-IDR frame references its predecessor).
  if (awaiting_keyframe_ && !keyframe) {
    stale_at_decoder_ += 1;
    request_idr(last_good_seq_, now_ms);
    return;
  }
  awaiting_keyframe_ = false;
  last_good_seq_ = frame_seq;

  counters_.frames_received += 1;
  {
    std::lock_guard<std::mutex> lk(mu_);
    au_queue_.push({capture_ts, std::move(au), keyframe});
  }
  cv_.notify_one();
}

// Every IDR request the client sends passes through here, so §3.6's "at most one
// per 250 ms" holds across both sources (reassembler gaps and decoder-overrun
// drops). The request is re-issued at that cadence while recovery is pending, so
// a lost recovery IDR does not stall recovery permanently (§3.6).
void VideoReceiver::request_idr(std::uint32_t last_good, std::int64_t now_ms) {
  if (!on_idr_) return;
  if (last_idr_request_ms_ >= 0 && now_ms - last_idr_request_ms_ < kIdrMinIntervalMs) {
    return;
  }

  last_idr_request_ms_ = now_ms;
  on_idr_(last_good);
}

void VideoReceiver::prune(std::uint32_t newest) {
  while (!pending_.empty()) {
    auto oldest = pending_.begin();
    if (newest > kPruneKeep && oldest->first < newest - kPruneKeep) {
      pending_.erase(oldest);
    } else {
      break;
    }
  }
}

std::optional<AccessUnit> VideoReceiver::pop_au() {
  std::unique_lock<std::mutex> lk(mu_);
  cv_.wait(lk, [this] { return stop_ || !au_queue_.empty(); });
  if (au_queue_.empty()) {
    return std::nullopt;  // stopped and drained
  }
  AccessUnit au = std::move(au_queue_.front());
  au_queue_.pop();
  return au;
}

void VideoReceiver::stop() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    stop_ = true;
  }
  cv_.notify_all();
}

void VideoReceiver::resume() {
  std::lock_guard<std::mutex> lk(mu_);
  stop_ = false;
}

}  // namespace loom::core
