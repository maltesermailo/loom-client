#include "loom/core/video_receiver.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "loom/proto/datagram.hpp"

using loom::core::AccessUnit;
using loom::core::VideoReceiver;

namespace {

// Build a single-fragment video datagram: §4.1 body = capture_ts (u64 BE) ‖ AU.
std::vector<std::uint8_t> video_datagram(std::uint32_t seq, bool keyframe, std::uint64_t capture_ts,
                                         std::vector<std::uint8_t> au) {
  std::vector<std::uint8_t> body;
  for (int i = 7; i >= 0; --i) body.push_back((capture_ts >> (i * 8)) & 0xFF);
  body.insert(body.end(), au.begin(), au.end());

  const auto h = loom::proto::make_header(keyframe, 0, seq, 0, 1);
  return loom::proto::encode_datagram(h, body);
}

// As above but on an arbitrary stream_id (multi-display, §4).
std::vector<std::uint8_t> video_datagram_on(std::uint16_t stream_id, std::uint32_t seq,
                                            bool keyframe, std::uint64_t capture_ts,
                                            std::vector<std::uint8_t> au) {
  std::vector<std::uint8_t> body;
  for (int i = 7; i >= 0; --i) body.push_back((capture_ts >> (i * 8)) & 0xFF);
  body.insert(body.end(), au.begin(), au.end());

  const auto h = loom::proto::make_header(keyframe, stream_id, seq, 0, 1);
  return loom::proto::encode_datagram(h, body);
}

// Drain every access unit currently queued (stop() first so pop_au() returns).
std::vector<AccessUnit> drain(VideoReceiver& rx) {
  rx.stop();
  std::vector<AccessUnit> out;
  while (auto au = rx.pop_au()) out.push_back(std::move(*au));
  return out;
}

}  // namespace

TEST_CASE("a stream_id-2 receiver assembles its stream and ignores others") {
  VideoReceiver rx([](std::uint32_t) {}, /*stream_id=*/2);
  // Datagrams for the primary (0) and audio (1) are not ours → ignored.
  rx.feed_datagram(video_datagram_on(0, 0, true, 1000, {0xAA}), 0);
  rx.feed_datagram(video_datagram_on(1, 0, false, 1000, {0xBB}), 0);
  // Our stream (2) assembles normally.
  rx.feed_datagram(video_datagram_on(2, 0, true, 5000, {0x11, 0x22}), 0);
  rx.feed_datagram(video_datagram_on(2, 1, false, 6000, {0x33}), 14);

  const auto aus = drain(rx);
  REQUIRE(aus.size() == 2);
  CHECK(aus[0].capture_ts == 5000);
  CHECK(aus[0].data == std::vector<std::uint8_t>{0x11, 0x22});
  CHECK(aus[1].capture_ts == 6000);
  CHECK(rx.counters().frames_received == 2);
  CHECK(rx.counters().datagrams == 2);  // the stream-0 and -1 datagrams were dropped
}

TEST_CASE("in-order frames assemble into access units, capture_ts stripped") {
  VideoReceiver rx([](std::uint32_t) {});
  rx.feed_datagram(video_datagram(0, true, 1000, {0xAA, 0xBB}), 0);
  rx.feed_datagram(video_datagram(1, false, 2000, {0xCC}), 14);

  const auto aus = drain(rx);
  REQUIRE(aus.size() == 2);
  CHECK(aus[0].capture_ts == 1000);
  CHECK(aus[0].keyframe);
  CHECK(aus[0].data == std::vector<std::uint8_t>{0xAA, 0xBB});
  CHECK(aus[1].capture_ts == 2000);
  CHECK(!aus[1].keyframe);
  CHECK(aus[1].data == std::vector<std::uint8_t>{0xCC});
  CHECK(rx.counters().frames_received == 2);
}

TEST_CASE("resume() revives the receiver for a swapped decoder") {
  // A mid-session resolution change (§8) swaps the decoder: HevcDecoder::stop()
  // stops this shared receiver to unblock the old decode thread, then resume()
  // must revive it so the new decoder keeps popping the continuing frame_seq.
  VideoReceiver rx([](std::uint32_t) {});
  rx.feed_datagram(video_datagram(0, true, 1000, {0xAA}), 0);

  rx.stop();
  CHECK(rx.pop_au().has_value());   // the queued keyframe drains
  CHECK(!rx.pop_au().has_value());  // stopped → nullopt (old decode thread exits)

  rx.resume();
  rx.feed_datagram(video_datagram(1, false, 2000, {0xBB}), 14);
  const auto au = rx.pop_au();  // no longer stopped: returns the next frame
  REQUIRE(au.has_value());
  CHECK(au->capture_ts == 2000);
  CHECK(au->data == std::vector<std::uint8_t>{0xBB});
}

TEST_CASE("a gap discards the frame and fires exactly one IDR request") {
  std::vector<std::uint32_t> idr_requests;
  VideoReceiver rx([&](std::uint32_t last_good) { idr_requests.push_back(last_good); });

  rx.feed_datagram(video_datagram(0, true, 0, {0x01}), 0);    // keyframe, delivered
  rx.feed_datagram(video_datagram(2, false, 0, {0x02}), 14);  // seq 1 missing → gap

  const auto aus = drain(rx);
  REQUIRE(aus.size() == 1);  // only the keyframe reached the decoder
  CHECK(aus[0].data == std::vector<std::uint8_t>{0x01});
  CHECK(idr_requests == std::vector<std::uint32_t>{0});  // one request, last_good = 0
  CHECK(rx.counters().frames_dropped == 1);
}

TEST_CASE("recovers after a gap: the keyframe is delivered and streaming resumes") {
  int idr_count = 0;
  VideoReceiver rx([&](std::uint32_t) { ++idr_count; });

  // Only one frame is in flight before the gap, so the freshness cap does not
  // fire (the real decode thread pops continuously); this isolates recovery.
  rx.feed_datagram(video_datagram(0, true, 0, {0x01}), 0);    // keyframe → delivered
  rx.feed_datagram(video_datagram(2, false, 0, {0x03}), 28);  // gap (1 missing) → discard + IDR
  rx.feed_datagram(video_datagram(3, true, 0, {0x04}), 300);  // recovery IDR → must deliver

  const auto aus = drain(rx);
  std::vector<std::vector<std::uint8_t>> data;
  for (const auto& au : aus) data.push_back(au.data);
  CHECK(data == std::vector<std::vector<std::uint8_t>>{{0x01}, {0x04}});
  CHECK(idr_count == 1);
}

TEST_CASE("IDR requests are rate-limited to 250 ms but retried while frozen (§3.6)") {
  int idr_count = 0;
  VideoReceiver rx([&](std::uint32_t) { ++idr_count; });

  rx.feed_datagram(video_datagram(0, true, 0, {0x01}), 0);
  rx.feed_datagram(video_datagram(2, false, 0, {0x02}), 14);   // gap → IDR #1
  rx.feed_datagram(video_datagram(3, false, 0, {0x03}), 100);  // still frozen, <250 ms → suppressed
  rx.feed_datagram(video_datagram(4, false, 0, {0x04}),
                   400);  // still frozen, >250 ms → IDR #2 (retry)

  // No recovery keyframe has arrived, so the client re-requests at the 250 ms
  // cadence rather than deadlocking on a single (possibly lost) IDR.
  CHECK(idr_count == 2);
}
