#include "loom/proto/clocksync.hpp"

namespace loom::proto::clocksync {

// Floor division by 2 (toward negative infinity). C++ '/' truncates toward
// zero, so adjust down by one for a negative, non-exact numerator.
static std::int64_t floor_div2(std::int64_t n) {
  std::int64_t q = n / 2;
  if (n % 2 != 0 && n < 0) q -= 1;
  return q;
}

Estimate ClockFilter::push(std::int64_t t0, std::int64_t t1, std::int64_t t2, std::int64_t t3) {
  const std::int64_t rtt = (t3 - t0) - (t2 - t1);
  const std::int64_t offset = floor_div2((t1 - t0) + (t2 - t3));

  window_.push_back(Estimate{rtt, offset});
  if (window_.size() > kWindow) window_.pop_front();

  // Min rtt over the window; iterating oldest->newest with '<=' lets the more
  // recent sample win a tie.
  Estimate best = window_.front();
  for (const auto& s : window_)
    if (s.rtt <= best.rtt) best = s;
  return best;
}

} // namespace loom::proto::clocksync
