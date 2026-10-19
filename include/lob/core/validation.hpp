#pragma once

// Stateless command validation: checks that need only the command and the
// static config. Checks that depend on book state (duplicate id, unknown id,
// capacity) belong to the matching engine.
//
// The order of checks is part of the observable behaviour: when several
// fields are invalid, the first failing check in the documented order is
// reported. Tests pin this order.

#include <optional>

#include "lob/core/command.hpp"
#include "lob/core/config.hpp"
#include "lob/core/reject_reason.hpp"
#include "lob/core/types.hpp"

namespace lob {

/// Returns std::nullopt if the order passes all stateless checks.
/// Check order: id, side, price band, quantity > 0, quantity <= max.
/// Precondition: config satisfies validate_config().
[[nodiscard]] constexpr std::optional<RejectReason> validate_new_order(
    const NewOrder& order, const EngineConfig& config) noexcept {
  if (order.id == kInvalidOrderId) {
    return RejectReason::InvalidOrderId;
  }
  if (!is_valid(order.side)) {
    return RejectReason::InvalidSide;
  }
  if (order.price < config.min_price || order.price > config.max_price) {
    return RejectReason::PriceOutOfRange;
  }
  if (order.quantity <= 0) {
    return RejectReason::InvalidQuantity;
  }
  if (order.quantity > config.max_order_quantity) {
    return RejectReason::QuantityTooLarge;
  }
  return std::nullopt;
}

/// Returns std::nullopt if the cancel passes all stateless checks.
[[nodiscard]] constexpr std::optional<RejectReason> validate_cancel_order(
    const CancelOrder& cancel) noexcept {
  if (cancel.id == kInvalidOrderId) {
    return RejectReason::InvalidOrderId;
  }
  return std::nullopt;
}

}  // namespace lob
