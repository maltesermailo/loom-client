#pragma once
// One client-side monotonic clock (µs). Used for CLOCK_PING t0, CLOCK_PONG t3,
// and the end-to-end latency computation — all must share one clock.

#include <chrono>
#include <cstdint>

namespace loom::sdl {

inline std::int64_t now_us() {
  static const auto epoch = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                               epoch)
      .count();
}

}  // namespace loom::sdl
