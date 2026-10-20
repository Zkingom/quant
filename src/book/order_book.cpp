#include "lob/book/order_book.hpp"

#include <iterator>
#include <stdexcept>
#include <string>

#include "lob/core/checked.hpp"
#include "lob/core/reject_reason.hpp"
#include "lob/core/validation.hpp"

namespace lob {

namespace {

[[noreturn]] void contract_violation(const char* where, const std::string& what) {
  throw std::logic_error(std::string("OrderBook::") + where + ": " + what);
}

}  // namespace

OrderBook::OrderBook(const EngineConfig& config) : config_(config) {
  validate_config(config_);
}

// ------------------------------------------------------------------ mutations

void OrderBook::add(const NewOrder& order, SeqNum seq) {
  // All checks happen before any mutation, so a throw leaves the book as is.
  if (const auto reject = validate_new_order(order, config_)) {
    contract_violation("add", "invalid order (" + std::string(to_string(*reject)) + ")");
  }
  if (index_.contains(order.id)) {
    contract_violation("add", "order id " + std::to_string(order.id) + " is already resting");
  }
  if (index_.size() >= static_cast<std::size_t>(config_.max_live_orders)) {
    contract_violation("add", "max_live_orders reached");
  }
  if (seq <= last_seq_) {
    contract_violation("add", "seq " + std::to_string(seq) + " is not greater than last seq " +
                                  std::to_string(last_seq_));
  }
  const auto opposite_best = best_price(opposite(order.side));
  if (opposite_best) {
    const bool crosses =
        order.side == Side::Buy ? order.price >= *opposite_best : order.price <= *opposite_best;
    if (crosses) {
      contract_violation("add", "order at " + std::to_string(order.price) +
                                    " would cross the opposite best " +
                                    std::to_string(*opposite_best));
    }
  }

  visit_side(order.side, [&](auto& levels) { insert(levels, order, seq); });
}

template <typename Levels>
void OrderBook::insert(Levels& levels, const NewOrder& order, SeqNum seq) {
  // Each step that can throw (allocation) is undone if a later step throws,
  // giving the strong guarantee promised by add().
  const auto [level_it, level_created] = levels.try_emplace(order.price);
  PriceLevel& level = level_it->second;

  try {
    level.orders.push_back(RestingOrder{.id = order.id, .remaining = order.quantity, .seq = seq});
  } catch (...) {
    if (level_created) {
      levels.erase(level_it);
    }
    throw;
  }

  try {
    index_.emplace(order.id, Locator{.side = order.side,
                                     .price = order.price,
                                     .level = &level,
                                     .order = std::prev(level.orders.end())});
  } catch (...) {
    level.orders.pop_back();
    if (level_created) {
      levels.erase(level_it);
    }
    throw;
  }

  // Cannot overflow: total <= max_live_orders * max_order_quantity (D-006).
  level.total_quantity += order.quantity;
  last_seq_ = seq;
}

Fill OrderBook::fill_front(Side side, Quantity quantity) {
  if (!is_valid(side)) {
    contract_violation("fill_front", "invalid side");
  }
  return visit_side(side, [&](auto& levels) -> Fill {
    if (levels.empty()) {
      contract_violation("fill_front", std::string(to_string(side)) + " side is empty");
    }
    const auto level_it = levels.begin();
    const Price price = level_it->first;
    PriceLevel& level = level_it->second;
    RestingOrder& maker = level.orders.front();

    if (quantity <= 0 || quantity > maker.remaining) {
      contract_violation("fill_front", "quantity " + std::to_string(quantity) +
                                           " not in (0, " + std::to_string(maker.remaining) + "]");
    }

    maker.remaining -= quantity;
    level.total_quantity -= quantity;
    const Fill fill{
        .maker_id = maker.id, .price = price, .quantity = quantity, .maker_remaining = maker.remaining};

    if (maker.remaining == 0) {
      index_.erase(maker.id);    // before pop_front: `maker` is still alive here
      level.orders.pop_front();  // `maker` is dangling from here on
      if (level.orders.empty()) {
        levels.erase(level_it);  // `level` is dangling from here on
      }
    }
    return fill;
  });
}

std::optional<CancelledOrder> OrderBook::cancel(OrderId id) noexcept {
  const auto entry = index_.find(id);
  if (entry == index_.end()) {
    return std::nullopt;
  }
  const Locator loc = entry->second;  // copy: the entry is erased below
  const CancelledOrder result{
      .id = id, .side = loc.side, .price = loc.price, .remaining = loc.order->remaining};

  loc.level->total_quantity -= result.remaining;
  loc.level->orders.erase(loc.order);
  index_.erase(entry);
  if (loc.level->orders.empty()) {
    visit_side(loc.side, [&](auto& levels) { levels.erase(loc.price); });
  }
  return result;
}

// -------------------------------------------------------------------- queries

std::optional<OrderView> OrderBook::find(OrderId id) const noexcept {
  const auto entry = index_.find(id);
  if (entry == index_.end()) {
    return std::nullopt;
  }
  const Locator& loc = entry->second;
  return OrderView{.id = id,
                   .side = loc.side,
                   .price = loc.price,
                   .remaining = loc.order->remaining,
                   .seq = loc.order->seq};
}

std::optional<Price> OrderBook::best_price(Side side) const noexcept {
  return visit_side(side, [](const auto& levels) -> std::optional<Price> {
    if (levels.empty()) {
      return std::nullopt;
    }
    return levels.begin()->first;
  });
}

std::optional<OrderView> OrderBook::front(Side side) const noexcept {
  return visit_side(side, [side](const auto& levels) -> std::optional<OrderView> {
    if (levels.empty()) {
      return std::nullopt;
    }
    const auto& [price, level] = *levels.begin();
    const RestingOrder& order = level.orders.front();
    return OrderView{.id = order.id,
                     .side = side,
                     .price = price,
                     .remaining = order.remaining,
                     .seq = order.seq};
  });
}

Quantity OrderBook::quantity_at(Side side, Price price) const noexcept {
  return visit_side(side, [price](const auto& levels) -> Quantity {
    const auto it = levels.find(price);
    return it == levels.end() ? 0 : it->second.total_quantity;
  });
}

std::size_t OrderBook::level_count(Side side) const noexcept {
  return visit_side(side, [](const auto& levels) { return levels.size(); });
}

std::vector<LevelSummary> OrderBook::depth(Side side, std::size_t max_levels) const {
  std::vector<LevelSummary> result;
  visit_side(side, [&](const auto& levels) {
    for (const auto& [price, level] : levels) {
      if (result.size() == max_levels) {
        break;
      }
      result.push_back(LevelSummary{
          .price = price, .total_quantity = level.total_quantity, .order_count = level.orders.size()});
    }
  });
  return result;
}

// ----------------------------------------------------------------- invariants

std::optional<std::string> OrderBook::find_invariant_violation() const {
  std::size_t orders_seen = 0;

  const auto check_side = [&](const auto& levels, Side side) -> std::optional<std::string> {
    for (const auto& [price, level] : levels) {
      const std::string where = std::string(to_string(side)) + " level " + std::to_string(price);
      if (price < config_.min_price || price > config_.max_price) {
        return where + ": price outside the configured band";
      }
      if (level.orders.empty()) {
        return where + ": empty level";
      }

      Quantity sum = 0;
      std::optional<SeqNum> previous_seq;
      for (const RestingOrder& order : level.orders) {
        const std::string who = where + " order " + std::to_string(order.id);
        if (order.remaining <= 0 || order.remaining > config_.max_order_quantity) {
          return who + ": remaining " + std::to_string(order.remaining) + " out of range";
        }
        if (previous_seq && order.seq <= *previous_seq) {
          return who + ": seq " + std::to_string(order.seq) + " breaks FIFO order";
        }
        if (order.seq > last_seq_) {
          return who + ": seq " + std::to_string(order.seq) + " exceeds last added seq";
        }
        previous_seq = order.seq;

        // Checked: a corrupted book may hold more orders than the config
        // bound that normally rules overflow out.
        const auto new_sum = checked_add(sum, order.remaining);
        if (!new_sum) {
          return where + ": quantity sum overflows";
        }
        sum = *new_sum;

        const auto entry = index_.find(order.id);
        if (entry == index_.end()) {
          return who + ": missing from index";
        }
        const Locator& loc = entry->second;
        if (loc.side != side || loc.price != price || loc.level != &level ||
            &*loc.order != &order) {
          return who + ": index entry does not point at this order";
        }
        ++orders_seen;
      }
      if (sum != level.total_quantity) {
        return where + ": total_quantity " + std::to_string(level.total_quantity) +
               " != sum of remaining " + std::to_string(sum);
      }
    }
    return std::nullopt;
  };

  if (auto violation = check_side(bids_, Side::Buy)) {
    return violation;
  }
  if (auto violation = check_side(asks_, Side::Sell)) {
    return violation;
  }
  // Each order seen above matched a distinct entry (entries point at distinct
  // nodes), so equal counts make the index a bijection.
  if (orders_seen != index_.size()) {
    return "index has " + std::to_string(index_.size()) + " entries but the book holds " +
           std::to_string(orders_seen) + " orders";
  }
  if (index_.size() > static_cast<std::size_t>(config_.max_live_orders)) {
    return "order count exceeds max_live_orders";
  }
  const auto bid = best_bid();
  const auto ask = best_ask();
  if (bid && ask && *bid >= *ask) {
    return "book is crossed: best bid " + std::to_string(*bid) + " >= best ask " +
           std::to_string(*ask);
  }
  return std::nullopt;
}

}  // namespace lob
