#pragma once

// ReferenceBook: an executable specification of OrderBook for differential
// testing. Deliberately naive and structurally unrelated to OrderBook:
//   - all live orders in ONE flat vector, in insertion order;
//   - no per-level containers, no index, no cached totals;
//   - every query is a linear scan; priority is computed by sorting.
// It shares with OrderBook only the value types and M1's stateless
// validate_new_order(). Performance is irrelevant; obviousness is the goal.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "lob/book/order_book.hpp"
#include "lob/core/validation.hpp"

namespace lob::test {

class ReferenceBook {
 public:
  explicit ReferenceBook(const EngineConfig& config) : config_(config) {}

  // ------------------------------------------------- price-time priority

  /// True if `a` trades before `b` (same side assumed).
  static bool higher_priority(const OrderView& a, const OrderView& b) {
    if (a.price != b.price) {
      return a.side == Side::Buy ? a.price > b.price : a.price < b.price;
    }
    return a.seq < b.seq;
  }

  [[nodiscard]] std::vector<OrderView> orders_in_priority(Side side) const {
    std::vector<OrderView> result;
    for (const OrderView& o : orders_) {
      if (o.side == side) {
        result.push_back(o);
      }
    }
    std::sort(result.begin(), result.end(), higher_priority);
    return result;
  }

  // ---------------------------------------------------------- queries

  [[nodiscard]] std::optional<OrderView> find(OrderId id) const {
    for (const OrderView& o : orders_) {
      if (o.id == id) {
        return o;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<OrderView> front(Side side) const {
    const auto sorted = orders_in_priority(side);
    if (sorted.empty()) {
      return std::nullopt;
    }
    return sorted.front();
  }

  [[nodiscard]] std::optional<Price> best_price(Side side) const {
    const auto f = front(side);
    return f ? std::optional<Price>(f->price) : std::nullopt;
  }

  [[nodiscard]] Quantity quantity_at(Side side, Price price) const {
    Quantity total = 0;
    for (const OrderView& o : orders_) {
      if (o.side == side && o.price == price) {
        total += o.remaining;
      }
    }
    return total;
  }

  [[nodiscard]] std::vector<LevelSummary> depth(Side side) const {
    std::vector<LevelSummary> levels;
    for (const OrderView& o : orders_in_priority(side)) {
      if (levels.empty() || levels.back().price != o.price) {
        levels.push_back(LevelSummary{.price = o.price, .total_quantity = 0, .order_count = 0});
      }
      levels.back().total_quantity += o.remaining;
      levels.back().order_count += 1;
    }
    return levels;
  }

  [[nodiscard]] std::size_t order_count() const { return orders_.size(); }
  [[nodiscard]] const std::vector<OrderView>& orders() const { return orders_; }

  // ------------------------------------------- contract (preconditions)

  [[nodiscard]] bool can_add(const NewOrder& order, SeqNum seq) const {
    if (validate_new_order(order, config_)) {
      return false;
    }
    if (find(order.id)) {
      return false;
    }
    if (static_cast<std::int64_t>(orders_.size()) >= config_.max_live_orders) {
      return false;
    }
    if (seq <= last_seq_) {
      return false;
    }
    const auto opposite_best = best_price(opposite(order.side));
    if (opposite_best) {
      if (order.side == Side::Buy && order.price >= *opposite_best) {
        return false;
      }
      if (order.side == Side::Sell && order.price <= *opposite_best) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] bool can_fill(Side side, Quantity quantity) const {
    if (!is_valid(side)) {
      return false;
    }
    const auto f = front(side);
    return f && quantity > 0 && quantity <= f->remaining;
  }

  // ------------------------------------------------------- mutations
  // Preconditions are the corresponding can_*(); callers check first.

  void add(const NewOrder& order, SeqNum seq) {
    orders_.push_back(OrderView{.id = order.id,
                                .side = order.side,
                                .price = order.price,
                                .remaining = order.quantity,
                                .seq = seq});
    last_seq_ = seq;
  }

  Fill fill(Side side, Quantity quantity) {
    const OrderView maker = *front(side);
    const auto it = position_of(maker.id);
    it->remaining -= quantity;
    const Fill result{.maker_id = maker.id,
                      .price = maker.price,
                      .quantity = quantity,
                      .maker_remaining = it->remaining};
    if (it->remaining == 0) {
      orders_.erase(it);
    }
    return result;
  }

  std::optional<CancelledOrder> cancel(OrderId id) {
    const auto it = position_of(id);
    if (it == orders_.end()) {
      return std::nullopt;
    }
    const CancelledOrder result{
        .id = it->id, .side = it->side, .price = it->price, .remaining = it->remaining};
    orders_.erase(it);
    return result;
  }

 private:
  std::vector<OrderView>::iterator position_of(OrderId id) {
    return std::find_if(orders_.begin(), orders_.end(),
                        [id](const OrderView& o) { return o.id == id; });
  }

  EngineConfig config_;
  std::vector<OrderView> orders_;
  SeqNum last_seq_{0};
};

}  // namespace lob::test
