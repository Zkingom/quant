#pragma once

// M2: the order book data structure.
//
// OrderBook stores resting orders in price-time priority and maintains an
// order-id index. It contains NO matching logic: the matching engine (M3)
// decides what to add, fill and cancel; the book only guarantees that its
// structure stays consistent (see find_invariant_violation()).
//
// Layout (DESIGN.md §2):
//   bids_  : std::map<Price, PriceLevel, std::greater<>>   begin() = best bid
//   asks_  : std::map<Price, PriceLevel, std::less<>>      begin() = best ask
//   index_ : std::unordered_map<OrderId, Locator>
//   PriceLevel = { total_quantity, std::list<RestingOrder> (FIFO) }
//
// Locator caches a PriceLevel* and a list iterator. Both stay valid until
// that exact order / level is erased, because std::map and std::list never
// relocate nodes on insertion or on erasure of other elements. Every erase
// path removes the index entry together with the order, so the index never
// holds a dangling locator.
//
// Contract checks (DECISIONS D-012): mutating calls verify their O(1)
// preconditions and throw std::logic_error on violation, leaving the book
// unchanged. A violation means the caller (the engine) has a bug; it is not a
// normal input error. Queries taking a Side require is_valid(side).

#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "lob/core/command.hpp"
#include "lob/core/config.hpp"
#include "lob/core/types.hpp"

namespace lob {

/// An order as stored inside a price level. Side and price are implied by
/// the level that holds it.
struct RestingOrder {
  OrderId id{kInvalidOrderId};
  Quantity remaining{0};
  SeqNum seq{0};
};

/// Self-contained copy of a resting order, returned by queries.
struct OrderView {
  OrderId id{kInvalidOrderId};
  Side side{Side::Buy};
  Price price{0};
  Quantity remaining{0};
  SeqNum seq{0};

  friend bool operator==(const OrderView&, const OrderView&) = default;
};

/// Result of fill_front(): which maker traded, at what price, how much, and
/// what is left of it (0 means it was removed from the book).
struct Fill {
  OrderId maker_id{kInvalidOrderId};
  Price price{0};
  Quantity quantity{0};
  Quantity maker_remaining{0};

  friend bool operator==(const Fill&, const Fill&) = default;
};

/// Result of a successful cancel(): the order as it was when removed.
struct CancelledOrder {
  OrderId id{kInvalidOrderId};
  Side side{Side::Buy};
  Price price{0};
  Quantity remaining{0};

  friend bool operator==(const CancelledOrder&, const CancelledOrder&) = default;
};

/// Aggregated view of one price level.
struct LevelSummary {
  Price price{0};
  Quantity total_quantity{0};
  std::size_t order_count{0};

  friend bool operator==(const LevelSummary&, const LevelSummary&) = default;
};

class OrderBook {
 public:
  /// Throws std::invalid_argument if the config is invalid (validate_config).
  explicit OrderBook(const EngineConfig& config);

  // Locators point into this object's own containers, so a member-wise copy
  // would alias the source book. Copying is therefore disabled.
  //
  // Moving is safe: std::map / std::list / std::unordered_map move
  // construction and (std::allocator) move assignment transfer nodes without
  // relocating them, so every Locator keeps pointing at the same nodes, now
  // owned by the destination. The standard leaves moved-from containers in
  // a "valid but unspecified" state, so the move operations explicitly clear
  // the source: afterwards it is an empty, consistent book with the same
  // config and last seq, usable like any other book (DECISIONS D-013).
  OrderBook(const OrderBook&) = delete;
  OrderBook& operator=(const OrderBook&) = delete;
  OrderBook(OrderBook&& other) noexcept;
  OrderBook& operator=(OrderBook&& other) noexcept;
  ~OrderBook() = default;

  // ---------------------------------------------------------------- mutations

  /// Rests `order` at the back of its price level's FIFO queue.
  /// Preconditions (std::logic_error otherwise, book unchanged):
  ///   - validate_new_order(order, config) passes;
  ///   - order.id is not already resting;
  ///   - order_count() < max_live_orders;
  ///   - seq is greater than the seq of every previous add();
  ///   - the order does not cross: a buy is below the best ask, a sell is
  ///     above the best bid.
  /// Strong exception guarantee (including std::bad_alloc).
  void add(const NewOrder& order, SeqNum seq);

  /// Executes `quantity` against the oldest order at the best price on
  /// `side` (the resting/maker side). Removes the order when it reaches zero
  /// and the level when it becomes empty.
  /// Preconditions (std::logic_error otherwise, book unchanged):
  ///   is_valid(side), side not empty, 0 < quantity <= front(side)->remaining.
  Fill fill_front(Side side, Quantity quantity);

  /// Removes a resting order. Returns std::nullopt if `id` is not resting
  /// (normal input condition, not a contract violation).
  std::optional<CancelledOrder> cancel(OrderId id) noexcept;

  // ------------------------------------------------------------------ queries

  [[nodiscard]] bool contains(OrderId id) const noexcept { return index_.contains(id); }
  [[nodiscard]] std::optional<OrderView> find(OrderId id) const noexcept;

  [[nodiscard]] std::optional<Price> best_price(Side side) const noexcept;
  [[nodiscard]] std::optional<Price> best_bid() const noexcept { return best_price(Side::Buy); }
  [[nodiscard]] std::optional<Price> best_ask() const noexcept { return best_price(Side::Sell); }

  /// Oldest order at the best price on `side`, i.e. the next maker to trade.
  [[nodiscard]] std::optional<OrderView> front(Side side) const noexcept;

  /// Total resting quantity at (side, price); 0 if there is no such level.
  [[nodiscard]] Quantity quantity_at(Side side, Price price) const noexcept;

  [[nodiscard]] std::size_t order_count() const noexcept { return index_.size(); }
  [[nodiscard]] std::size_t level_count(Side side) const noexcept;
  [[nodiscard]] bool empty() const noexcept { return index_.empty(); }

  /// Up to `max_levels` levels on `side`, best first.
  [[nodiscard]] std::vector<LevelSummary> depth(Side side, std::size_t max_levels) const;

  /// Calls visit(Price, const RestingOrder&) for every order on `side` in
  /// priority order: best price first, FIFO within a price. This is the
  /// deterministic traversal used for snapshots; the hash index is never
  /// iterated for output.
  template <typename F>
  void for_each_order(Side side, F&& visit) const {
    visit_side(side, [&](const auto& levels) {
      for (const auto& [price, level] : levels) {
        for (const RestingOrder& order : level.orders) {
          visit(price, order);
        }
      }
    });
  }

  /// Like for_each_order, but stops as soon as visit(Price, const RestingOrder&)
  /// returns false. Visits only the orders it needs to, in priority order;
  /// the matching engine uses it to plan a match in O(orders touched)
  /// without modifying the book (DECISIONS D-018).
  template <typename F>
  void for_each_order_while(Side side, F&& visit) const {
    visit_side(side, [&](const auto& levels) {
      for (const auto& [price, level] : levels) {
        for (const RestingOrder& order : level.orders) {
          if (!visit(price, order)) {
            return;
          }
        }
      }
    });
  }

  [[nodiscard]] const EngineConfig& config() const noexcept { return config_; }

  /// O(n) full consistency check. Returns a description of the first
  /// violated invariant, or std::nullopt if the book is consistent:
  ///   1. no empty price level; every level price inside the price band;
  ///   2. level total_quantity == sum of its orders' remaining, and every
  ///      remaining is in (0, max_order_quantity];
  ///   3. seq strictly increasing along each FIFO queue, and <= last add seq;
  ///   4. index is a bijection onto resting orders: every order has an entry
  ///      pointing at exactly that node, level, side and price, and there are
  ///      no extra entries;
  ///   5. order_count() <= max_live_orders;
  ///   6. not crossed: best_bid < best_ask when both sides are non-empty.
  /// Independent of NDEBUG; tests call it after every operation.
  [[nodiscard]] std::optional<std::string> find_invariant_violation() const;

 private:
  // Defined only under tests/ (tamper tests corrupt state on purpose to prove
  // that find_invariant_violation() detects it). Adds no member, no public
  // API and no runtime cost; production code never defines it (D-015).
  friend struct OrderBookTestAccess;

  using OrderList = std::list<RestingOrder>;

  struct PriceLevel {
    Quantity total_quantity{0};
    OrderList orders;
  };

  using BidLevels = std::map<Price, PriceLevel, std::greater<>>;
  using AskLevels = std::map<Price, PriceLevel, std::less<>>;

  // The two maps have different comparator types, hence different iterator
  // types; a PriceLevel* (node-stable) plus the price key serves both sides.
  struct Locator {
    Side side{Side::Buy};
    Price price{0};
    PriceLevel* level{nullptr};
    OrderList::iterator order;
  };

  template <typename F>
  decltype(auto) visit_side(Side side, F&& f) {
    if (side == Side::Buy) {
      return f(bids_);
    }
    return f(asks_);
  }

  template <typename F>
  decltype(auto) visit_side(Side side, F&& f) const {
    if (side == Side::Buy) {
      return f(bids_);
    }
    return f(asks_);
  }

  template <typename Levels>
  void insert(Levels& levels, const NewOrder& order, SeqNum seq);

  EngineConfig config_;
  BidLevels bids_;
  AskLevels asks_;
  std::unordered_map<OrderId, Locator> index_;
  SeqNum last_seq_{0};
};

}  // namespace lob
