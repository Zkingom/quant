#pragma once

// Input commands accepted by the matching engine.
//
// Commands are plain aggregates. Construct them with designated initializers
// so that field order mistakes (price vs. quantity) are impossible at the
// call site:
//
//   NewOrder{.id = 7, .side = Side::Sell, .price = 101, .quantity = 5}
//
// Every field defaults to a value that stateless validation rejects (id 0,
// price 0, quantity 0), so a forgotten field cannot produce a valid order.
// (Side has no invalid default; a forgotten side is Buy. The id/price/quantity
// defaults still guarantee rejection.)

#include <variant>

#include "lob/core/types.hpp"

namespace lob {

/// Good-till-cancel limit order. Any unfilled remainder rests on the book.
struct NewOrder {
  OrderId id{kInvalidOrderId};
  Side side{Side::Buy};
  Price price{0};
  Quantity quantity{0};

  friend constexpr bool operator==(const NewOrder&, const NewOrder&) = default;
};

/// Cancel the full remaining quantity of a live order.
struct CancelOrder {
  OrderId id{kInvalidOrderId};

  friend constexpr bool operator==(const CancelOrder&, const CancelOrder&) = default;
};

using Command = std::variant<NewOrder, CancelOrder>;

}  // namespace lob
