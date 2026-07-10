#pragma once
// Clock synchronization min-filter — PROTOCOL.md §7.
//
// From each PING/PONG exchange with client times t0 (send), t3 (PONG receive)
// and host times t1, t2:
//   rtt    = (t3 - t0) - (t2 - t1)
//   offset = floor(((t1 - t0) + (t2 - t3)) / 2)     // host_time ~= client_time + offset
// All arithmetic is signed 64-bit microseconds; the division floors toward
// negative infinity. The current estimate is the (rtt, offset) of the
// minimum-rtt sample over the last 16 samples, ties won by the more recent one.
#include <cstdint>
#include <deque>

namespace loom::proto::clocksync {

inline constexpr std::size_t kWindow = 16;

struct Estimate {
  std::int64_t rtt;
  std::int64_t offset;
  bool operator==(const Estimate&) const = default;
};

class ClockFilter {
public:
  // Incorporate one sample and return the current estimate after it.
  // t0, t3 are client-clock us (ping send / pong receive); t1, t2 are host-clock
  // us (host receive / host send).
  Estimate push(std::int64_t t0, std::int64_t t1, std::int64_t t2, std::int64_t t3);

private:
  std::deque<Estimate> window_;
};

} // namespace loom::proto::clocksync
