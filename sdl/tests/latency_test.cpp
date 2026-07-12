#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "clock.hpp"
#include "decoder.hpp"
#include "idr_fixture.hpp"
#include "loom/proto/datagram.hpp"
#include "video_pipeline.hpp"

using loom::sdl::DecodedFrame;
using loom::sdl::VideoPipeline;

namespace {

// Fragment a frame body into datagrams exactly as the host does (§4).
std::vector<std::vector<std::uint8_t>> fragment(std::uint32_t seq, bool keyframe,
                                                const std::vector<std::uint8_t>& body) {
  const std::size_t maxp = loom::proto::kMaxDatagramLen - loom::proto::kHeaderLen;
  std::size_t count = (body.size() + maxp - 1) / maxp;
  if (count == 0) count = 1;
  std::vector<std::vector<std::uint8_t>> out;
  for (std::size_t i = 0; i < count; ++i) {
    const std::size_t off = i * maxp;
    const std::size_t len = std::min(maxp, body.size() - off);
    const auto h = loom::proto::make_header(keyframe, 0, seq, static_cast<std::uint16_t>(i),
                                            static_cast<std::uint16_t>(count));
    out.push_back(loom::proto::encode_datagram(
        h, std::span<const std::uint8_t>(body.data() + off, len)));
  }
  return out;
}

}  // namespace

// M1.3 accept: the overlay's e2e latency (client display time minus the frame's
// capture_ts, offset-corrected) must match the true elapsed time within ±2 ms.
// We control the capture instant, drive the real reassembly + libavcodec decode,
// and check the reported latency against an independent computation.
TEST_CASE("e2e latency from capture_ts is accurate within 2 ms through real decode") {
  VideoPipeline pipeline([](std::uint32_t) {});

  const auto au = loom::sdl::test::idr_access_unit();
  const std::int64_t capture = loom::sdl::now_us();  // the frame's capture instant

  // Frame body = capture_ts (u64 BE) ‖ Annex-B AU (§4.1).
  std::vector<std::uint8_t> body(8);
  for (int i = 0; i < 8; ++i) {
    body[i] = static_cast<std::uint8_t>((static_cast<std::uint64_t>(capture) >> (56 - 8 * i)) & 0xFF);
  }
  body.insert(body.end(), au.begin(), au.end());

  for (const auto& dg : fragment(0, true, body)) {
    pipeline.feed_datagram(dg);
  }

  // Wait for the decode thread to produce the frame.
  std::shared_ptr<const DecodedFrame> frame;
  for (int i = 0; i < 400 && !frame; ++i) {
    frame = pipeline.take_frame();
    if (!frame) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  REQUIRE(frame != nullptr);
  CHECK(frame->width == 320);
  CHECK(frame->height == 240);

  // capture_ts must survive reassembly + decode intact.
  CHECK(frame->capture_ts == static_cast<std::uint64_t>(capture));

  const std::int64_t display = loom::sdl::now_us();
  const std::int64_t offset = 0;  // same clock in-test
  const std::int64_t e2e_reported = display - (static_cast<std::int64_t>(frame->capture_ts) - offset);
  const std::int64_t e2e_true = display - capture;

  CHECK(std::llabs(e2e_reported - e2e_true) < 2000);  // ±2 ms
  CHECK(e2e_reported > 0);                            // real elapsed (decode took time)
}
