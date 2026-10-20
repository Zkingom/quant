#pragma once

// M3: events emitted by the matching engine.
//
// Every event carries a seq from one engine-wide counter that starts at 1
// and increases by exactly 1 per event (rejections included). An order's
// time priority on the book is the seq of its OrderAccepted event.
//
// Event sequences per command (DESIGN.md §3.2):
//   new order, accepted : Accepted, Trade*, [Rested]   (no Rested = fully filled)
//   new order, rejected : Rejected
//   cancel              : Cancelled(Requested) | Rejected
// plus, only when resting the remainder fails for lack of memory:
//                         Accepted, Trade*, Cancelled(InsufficientResources)
//
// All event types are trivially copyable so that appending an event to a
// vector with spare capacity cannot allocate or throw.

#include <cstdint>
#include <string_view>
#include <type_traits>
#include <variant>

#include "lob/core/reject_reason.hpp"
#include "lob/core/types.hpp"

namespace lob {

enum class CommandType : std::uint8_t { NewOrder, CancelOrder };

enum class CancelReason : std::uint8_t {
  Requested,              ///< CancelOrder command
  InsufficientResources,  ///< remainder could not be rested (std::bad_alloc)
};

struct OrderAccepted {
  SeqNum seq{0};
  OrderId id{kInvalidOrderId};
  Side side{Side::Buy};
  Price price{0};
  Quantity quantity{0};

  friend bool operator==(const OrderAccepted&, const OrderAccepted&) = default;
};

struct OrderRejected {
  SeqNum seq{0};
  CommandType command{CommandType::NewOrder};
  OrderId id{kInvalidOrderId};
  RejectReason reason{RejectReason::InvalidOrderId};

  friend bool operator==(const OrderRejected&, const OrderRejected&) = default;
};

/// One execution between the incoming order (taker) and one resting order
/// (maker). price is always the maker's price. The remaining quantities are
/// after this execution.
struct TradeExecuted {
  SeqNum seq{0};
  OrderId taker_id{kInvalidOrderId};
  OrderId maker_id{kInvalidOrderId};
  Side taker_side{Side::Buy};
  Price price{0};
  Quantity quantity{0};
  Quantity taker_remaining{0};
  Quantity maker_remaining{0};

  friend bool operator==(const TradeExecuted&, const TradeExecuted&) = default;
};

struct OrderRested {
  SeqNum seq{0};
  OrderId id{kInvalidOrderId};
  Side side{Side::Buy};
  Price price{0};
  Quantity remaining{0};

  friend bool operator==(const OrderRested&, const OrderRested&) = default;
};

struct OrderCancelled {
  SeqNum seq{0};
  OrderId id{kInvalidOrderId};
  Side side{Side::Buy};
  Price price{0};
  Quantity remaining{0};
  CancelReason reason{CancelReason::Requested};

  friend bool operator==(const OrderCancelled&, const OrderCancelled&) = default;
};

using Event = std::variant<OrderAccepted, OrderRejected, TradeExecuted, OrderRested, OrderCancelled>;

static_assert(std::is_trivially_copyable_v<OrderAccepted>);
static_assert(std::is_trivially_copyable_v<OrderRejected>);
static_assert(std::is_trivially_copyable_v<TradeExecuted>);
static_assert(std::is_trivially_copyable_v<OrderRested>);
static_assert(std::is_trivially_copyable_v<OrderCancelled>);
static_assert(std::is_nothrow_copy_constructible_v<Event>);

[[nodiscard]] constexpr SeqNum seq_of(const Event& event) noexcept {
  return std::visit([](const auto& e) { return e.seq; }, event);
}

/// Stable textual forms (used by tests now and by the M5 text format).
[[nodiscard]] constexpr std::string_view to_string(CommandType command) noexcept {
  switch (command) {
    case CommandType::NewOrder:
      return "NEW";
    case CommandType::CancelOrder:
      return "CANCEL";
  }
  return "INVALID";
}

[[nodiscard]] constexpr std::string_view to_string(CancelReason reason) noexcept {
  switch (reason) {
    case CancelReason::Requested:
      return "REQUESTED";
    case CancelReason::InsufficientResources:
      return "INSUFFICIENT_RESOURCES";
  }
  return "INVALID";
}

}  // namespace lob
