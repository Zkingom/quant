#pragma once

#include <cstdint>

#include "lob/core/types.hpp"

namespace lob {

/// Static limits of one engine instance. Validate once with validate_config()
/// before use; every other function in the library assumes a valid config.
struct EngineConfig {
  /// Inclusive price band, in ticks.
  Price min_price{1};
  Price max_price{1'000'000'000};

  /// Largest quantity a single order may carry.
  Quantity max_order_quantity{1'000'000'000};

  /// Upper bound on simultaneously resting orders (both sides). Bounds memory
  /// and, together with max_order_quantity, guarantees that any sum of
  /// resting quantities fits in Quantity (DECISIONS D-006).
  std::int64_t max_live_orders{10'000'000};

  friend constexpr bool operator==(const EngineConfig&, const EngineConfig&) = default;
};

/// Throws std::invalid_argument naming the first violated constraint:
///   1. min_price >= 1
///   2. max_price >= min_price
///   3. max_order_quantity >= 1
///   4. max_live_orders >= 1
///   5. max_price * max_order_quantity fits in int64     (trade notional)
///   6. max_live_orders * max_order_quantity fits in int64 (quantity sums)
void validate_config(const EngineConfig& config);

}  // namespace lob
