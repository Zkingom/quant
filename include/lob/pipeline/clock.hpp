#pragma once

// Timestamps for the latency waterfall (L3' stage 2). DESIGN §7.5.
//
// On x86-64 the clock is the TSC (rdtsc, ~ten cycles, no syscall, no vDSO);
// elsewhere std::chrono::steady_clock. Ticks convert to ns with a ratio
// calibrated against steady_clock once at start-up.
//
// Cross-thread caveat: a hop that starts on one core and ends on another
// (the SPSC queue hop) subtracts two different cores' TSC readings. With an
// invariant, synchronised TSC (constant_tsc + nonstop_tsc; the kernel checks
// synchronisation at boot) the offset is a few cycles, but under a
// hypervisor (WSL2, cloud VMs) the guest cannot verify it, and a vCPU can be
// descheduled between two reads. Hence: per-hop values are clamped at 0,
// the clock-read overhead is measured and subtracted, and the tail (p99.9)
// is reported as an upper bound that includes virtualisation noise.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

#if defined(__x86_64__)
#include <x86intrin.h>
#endif

namespace lob::pipeline {

[[nodiscard]] inline std::uint64_t steady_ns() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

/// Raw clock ticks (TSC on x86-64, ns elsewhere).
[[nodiscard]] inline std::uint64_t ticks() noexcept {
#if defined(__x86_64__)
  return __rdtsc();
#else
  return steady_ns();
#endif
}

struct ClockCalibration {
  double ns_per_tick{1.0};
  double overhead_ticks{0.0};  ///< median cost of one ticks() pair, i.e. of one hop measurement
  [[nodiscard]] double to_ns(std::uint64_t t) const noexcept { return static_cast<double>(t) * ns_per_tick; }
};

/// Measures ns/tick over ~`window_ms` and the read overhead (median of
/// back-to-back pairs). Not on the hot path.
[[nodiscard]] inline ClockCalibration calibrate(int window_ms = 50) {
  ClockCalibration c;
  const std::uint64_t s0 = steady_ns();
  const std::uint64_t t0 = ticks();
  while (steady_ns() - s0 < static_cast<std::uint64_t>(window_ms) * std::uint64_t{1'000'000}) {
  }
  const std::uint64_t s1 = steady_ns();
  const std::uint64_t t1 = ticks();
  c.ns_per_tick = t1 > t0 ? static_cast<double>(s1 - s0) / static_cast<double>(t1 - t0) : 1.0;
  std::vector<std::uint64_t> d(20001);
  for (auto& x : d) {
    const std::uint64_t a = ticks();
    const std::uint64_t b = ticks();
    x = b - a;
  }
  std::nth_element(d.begin(), d.begin() + static_cast<long>(d.size() / 2), d.end());
  c.overhead_ticks = static_cast<double>(d[d.size() / 2]);
  return c;
}

}  // namespace lob::pipeline
