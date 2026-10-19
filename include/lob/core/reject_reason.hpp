#pragma once

#include <cstdint>
#include <string_view>

namespace lob {

/// Why a command was rejected. Stateless reasons are produced by
/// validation.hpp; stateful reasons are produced by the matching engine,
/// which knows the current book contents.
enum class RejectReason : std::uint8_t {
  // Stateless (validation.hpp).
  InvalidOrderId,    ///< id == kInvalidOrderId
  InvalidSide,       ///< side is not Buy or Sell
  PriceOutOfRange,   ///< price outside [min_price, max_price]
  InvalidQuantity,   ///< quantity <= 0
  QuantityTooLarge,  ///< quantity > max_order_quantity

  // Stateful (matching engine).
  DuplicateOrderId,  ///< new order id collides with a live order
  UnknownOrderId,    ///< cancel for an id that is not live
  BookFull,          ///< accepting would exceed max_live_orders
};

/// Stable textual form used in event output and golden files. Do not change
/// the spelling of existing values. The switch has no default so that
/// -Wswitch flags any enumerator added without a string.
[[nodiscard]] constexpr std::string_view to_string(RejectReason reason) noexcept {
  switch (reason) {
    case RejectReason::InvalidOrderId:
      return "INVALID_ORDER_ID";
    case RejectReason::InvalidSide:
      return "INVALID_SIDE";
    case RejectReason::PriceOutOfRange:
      return "PRICE_OUT_OF_RANGE";
    case RejectReason::InvalidQuantity:
      return "INVALID_QUANTITY";
    case RejectReason::QuantityTooLarge:
      return "QUANTITY_TOO_LARGE";
    case RejectReason::DuplicateOrderId:
      return "DUPLICATE_ORDER_ID";
    case RejectReason::UnknownOrderId:
      return "UNKNOWN_ORDER_ID";
    case RejectReason::BookFull:
      return "BOOK_FULL";
  }
  return "UNKNOWN_REJECT_REASON";
}

}  // namespace lob
