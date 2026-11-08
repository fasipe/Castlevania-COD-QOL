#pragma once
#include <chrono>
#include <cstdint>
#include <intrin.h>

// Nanoseconds per tick of the processor's cycle counter, measured once.
inline double tscNanos() {
  static const double ns = [] {
    const auto t0 = std::chrono::steady_clock::now();
    const uint64_t c0 = __rdtsc();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(2)) {}
    const uint64_t c1 = __rdtsc();
    return double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()) / double(c1 - c0);
  }();
  return ns;
}

// Adds the time spent in a scope to a counter (nanoseconds); used for the run report. Runs around
// each VU program, so it reads the cycle counter rather than the system's clock.
class ScopedTimer {
public:
  explicit ScopedTimer(uint64_t& total) : total_(total), start_(__rdtsc()) {}
  ~ScopedTimer() { total_ += uint64_t(double(__rdtsc() - start_) * tscNanos()); }

private:
  uint64_t& total_;
  uint64_t start_;
};
