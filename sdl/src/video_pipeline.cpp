#include "video_pipeline.hpp"

#include <cstdio>
#include <utility>

#include "loom/proto/datagram.hpp"

namespace loom::sdl {

namespace reassembly = loom::proto::reassembly;

// Frame body prefix carrying the host capture timestamp (§4.1).
constexpr std::size_t kCaptureTsLen = 8;
// Keep a few frames of payloads around; older incomplete ones are dead (§6.2).
constexpr std::uint32_t kPruneKeep = 4;

VideoPipeline::VideoPipeline(IdrRequestFn on_idr) : on_idr_(std::move(on_idr)) {
  thread_ = std::thread([this] { decode_loop(); });
}

VideoPipeline::~VideoPipeline() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

std::int64_t VideoPipeline::now_ms() const {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - start_)
      .count();
}

void VideoPipeline::feed_datagram(std::span<const std::uint8_t> datagram) {
  auto decoded = loom::proto::decode(datagram);
  if (!decoded) {
    return;  // malformed → silent drop (§6.6)
  }
  const auto& h = decoded.value().header;
  if (h.stream_id != 0) {
    return;  // video only for now (audio is M5)
  }
  counters_.datagrams += 1;
  counters_.bytes += datagram.size();

  // Stash this fragment's payload for later assembly.
  auto& pending = pending_[h.frame_seq];
  pending.count = h.frag_count;
  pending.parts[h.frag_index] =
      std::vector<std::uint8_t>(datagram.begin() + loom::proto::kHeaderLen, datagram.end());

  // Feed header metadata to the receive-model state machine.
  reasm_.push(now_ms(),
              reassembly::Fragment{h.frame_seq, h.frag_index, h.frag_count, h.keyframe});

  const auto& events = reasm_.events();
  for (; seen_events_ < events.size(); ++seen_events_) {
    const auto& e = events[seen_events_];
    if (e.kind == reassembly::Event::Kind::Deliver) {
      deliver_frame(e.frame_seq);
    } else if (e.kind == reassembly::Event::Kind::IdrRequest && on_idr_) {
      on_idr_(e.last_good);
    }
  }
  // frames dropped = every reassembler drop category (loss / gap / stale).
  const auto& c = reasm_.counters();
  counters_.frames_dropped = c.dropped_incomplete + c.discarded_gap + c.stale_fragments;
  prune(h.frame_seq);
}

void VideoPipeline::deliver_frame(std::uint32_t frame_seq) {
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
  counters_.frames_received += 1;
  {
    std::lock_guard<std::mutex> lk(mu_);
    au_queue_.push({capture_ts, std::move(au)});
  }
  cv_.notify_one();
}

void VideoPipeline::prune(std::uint32_t newest) {
  while (!pending_.empty()) {
    auto oldest = pending_.begin();
    if (newest > kPruneKeep && oldest->first < newest - kPruneKeep) {
      pending_.erase(oldest);
    } else {
      break;
    }
  }
}

std::shared_ptr<const DecodedFrame> VideoPipeline::take_frame() {
  std::lock_guard<std::mutex> lk(mu_);
  if (!has_new_) {
    return nullptr;
  }
  has_new_ = false;
  return latest_;
}

void VideoPipeline::decode_loop() {
  HevcDecoder decoder;
  while (true) {
    QueuedAu item;
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [this] { return stop_ || !au_queue_.empty(); });
      if (stop_ && au_queue_.empty()) {
        return;
      }
      item = std::move(au_queue_.front());
      au_queue_.pop();
    }
    DecodedFrame frame;
    if (decoder.decode(item.data, frame)) {
      frame.capture_ts = item.capture_ts;
      auto shared = std::make_shared<const DecodedFrame>(std::move(frame));
      {
        std::lock_guard<std::mutex> lk(mu_);
        latest_ = shared;
        has_new_ = true;
      }
      std::fprintf(stderr, "{\"event\":\"frame_decoded\",\"w\":%d,\"h\":%d}\n", shared->width,
                   shared->height);
    }
  }
}

}  // namespace loom::sdl
