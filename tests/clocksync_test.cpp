#include <doctest/doctest.h>

#include "loom/proto/clocksync.hpp"

using loom::proto::clocksync::ClockFilter;
using loom::proto::clocksync::Estimate;

TEST_CASE("basic rtt/offset arithmetic") {
  ClockFilter f;
  CHECK(f.push(1000, 501500, 501540, 2100) == Estimate{1060, 499970});
}

TEST_CASE("the minimum-rtt sample wins across the window") {
  ClockFilter f;
  f.push(1000, 501500, 501540, 2100);                                     // rtt 1060
  CHECK(f.push(501000, 1002200, 1002240, 502400) == Estimate{1060, 499970}); // rtt 1360, min holds
  CHECK(f.push(1001000, 1501900, 1501940, 1002000) == Estimate{960, 500420}); // rtt 960 -> new best
}

TEST_CASE("offset floors toward negative infinity") {
  ClockFilter f;
  CHECK(f.push(0, -250000, -249960, 1000) == Estimate{960, -250480});
  // odd, negative numerator: ((0-0)+(0-3))/2 = -3/2 -> floor(-1.5) = -2
  ClockFilter g;
  CHECK(g.push(0, 0, 0, 3).offset == -2);
}

TEST_CASE("ties are won by the more recent sample") {
  ClockFilter f;
  f.push(0, 1000, 1040, 100);                    // rtt 60, offset 970
  const auto e = f.push(0, 2000, 2040, 100);     // rtt 60 again, offset 1970
  CHECK(e.rtt == 60);
  CHECK(e.offset == 1970);                       // the later sample's offset
}

TEST_CASE("window slides: an old minimum is evicted") {
  ClockFilter f;
  CHECK(f.push(0, 500, 540, 100).rtt == 60);     // a low-rtt sample
  for (int i = 0; i < 16; ++i) {                  // 16 more push it out of the window
    const std::int64_t base = 1000000 + i * 1000;
    f.push(base, base + 500, base + 600, base + 200); // rtt 100 each
  }
  CHECK(f.push(9000000, 9000500, 9000600, 9000200).rtt == 100); // best remaining is 100
}
