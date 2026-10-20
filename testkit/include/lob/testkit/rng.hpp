#pragma once

#include <cstdint>
#include <random>

#include "lob/core/types.hpp"

namespace lob::testkit {

/// Seeded generator for differential tests and workloads. Deterministic on every platform:
/// std::mt19937_64 is fully specified by the standard, and the range
/// reduction is ours (std::uniform_int_distribution's algorithm is not
/// specified and differs between standard libraries). Modulo bias is
/// irrelevant for test generation.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : engine_(seed) {}

  /// Uniform in [lo, hi], inclusive. Precondition: lo <= hi.
  std::int64_t uniform(std::int64_t lo, std::int64_t hi) {
    const auto span = static_cast<std::uint64_t>(hi - lo) + 1;
    return lo + static_cast<std::int64_t>(engine_() % span);
  }
  bool percent(int p) { return uniform(0, 99) < p; }
  Side side() { return percent(50) ? Side::Buy : Side::Sell; }

 private:
  std::mt19937_64 engine_;
};

}  // namespace lob::testkit
