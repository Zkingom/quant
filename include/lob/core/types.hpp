#pragma once

// Fundamental value types shared by every module.
//
// All quantities are integers: prices are expressed in ticks and quantities
// in lots, so comparisons and sums are exact and results are bit-for-bit
// reproducible across compilers and optimisation levels (DECISIONS D-001).

#include <cstdint>
#include <string_view>

namespace lob {

/// Limit price in integer ticks. Valid range is [EngineConfig::min_price,
/// EngineConfig::max_price]; see validation.hpp.
using Price = std::int64_t;

/// Order quantity in lots. Signed on purpose: an arithmetic mistake yields a
/// detectable negative value instead of silently wrapping (DECISIONS D-003).
using Quantity = std::int64_t;

/// Client-supplied order identifier. Zero is reserved and never valid.
using OrderId = std::uint64_t;

/// Engine-assigned, strictly increasing sequence number. It is the only
/// notion of time inside the engine: time priority means "lower SeqNum".
using SeqNum = std::uint64_t;

inline constexpr OrderId kInvalidOrderId = 0;

/// The underlying type is fixed, so any uint8_t value is a representable
/// Side (e.g. after decoding untrusted bytes). Only Buy and Sell are valid;
/// use is_valid() before relying on a Side from outside the engine.
enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

[[nodiscard]] constexpr bool is_valid(Side side) noexcept {
  return side == Side::Buy || side == Side::Sell;
}

/// Precondition: is_valid(side).
[[nodiscard]] constexpr Side opposite(Side side) noexcept {
  return side == Side::Buy ? Side::Sell : Side::Buy;
}

/// Stable textual form used in logs and golden files. Do not change the
/// spelling of existing values: golden outputs depend on it.
[[nodiscard]] constexpr std::string_view to_string(Side side) noexcept {
  switch (side) {
    case Side::Buy:
      return "BUY";
    case Side::Sell:
      return "SELL";
  }
  return "INVALID";
}

}  // namespace lob
