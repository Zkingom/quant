#pragma once

// Naive reference for lob::md::Book (L3' 1c): one std::map of live orders,
// every query a linear scan. It is the specification, not a product: it
// shares only the status enum and the order of checks with the real book
// (DESIGN §7.3 fixes that order; it is observable behaviour).

#include <algorithm>
#include <cstdint>
#include <map>
#include <utility>
#include <optional>
#include <vector>

#include "lob/md/book.hpp"

namespace lob::testkit {

class ReferenceMdBook {
 public:
  using Side = md::Side;
  using Status = md::BookStatus;

  ReferenceMdBook(std::size_t max_orders, std::size_t max_levels)
      : max_orders_(max_orders), max_levels_(max_levels) {}

  Status add(std::uint64_t ref, Side side, std::uint32_t shares, std::uint32_t price) {
    if (orders_.count(ref) != 0) return Status::DuplicateOrderRef;
    if (const Status s = check_new(side, shares, price, std::nullopt); s != Status::Applied) return s;
    if (orders_.size() >= max_orders_) return Status::OrderCapacity;
    orders_[ref] = Order{side, price, shares, next_arrival_++};
    return Status::Applied;
  }

  Status execute(std::uint64_t ref, std::uint32_t shares) {
    auto it = orders_.find(ref);
    if (it == orders_.end()) return Status::UnknownOrderRef;
    if (shares == 0) return Status::ZeroShares;
    if (shares > it->second.shares) return Status::Overfill;
    it->second.shares -= shares;
    if (it->second.shares == 0) orders_.erase(it);
    return Status::Applied;
  }
  Status cancel(std::uint64_t ref, std::uint32_t shares) { return execute(ref, shares); }

  Status remove(std::uint64_t ref) {
    return orders_.erase(ref) == 1 ? Status::Applied : Status::UnknownOrderRef;
  }

  Status replace(std::uint64_t old_ref, std::uint64_t new_ref, std::uint32_t shares, std::uint32_t price) {
    auto it = orders_.find(old_ref);
    if (it == orders_.end()) return Status::UnknownOrderRef;
    if (new_ref != old_ref && orders_.count(new_ref) != 0) return Status::DuplicateOrderRef;
    const Side side = it->second.side;
    if (const Status s = check_new(side, shares, price, old_ref); s != Status::Applied) return s;
    orders_.erase(it);
    orders_[new_ref] = Order{side, price, shares, next_arrival_++};
    return Status::Applied;
  }

  void clear() { orders_.clear(); }

  [[nodiscard]] std::size_t order_count() const { return orders_.size(); }

  /// Distinct prices on `side`, best first.
  [[nodiscard]] std::vector<std::uint32_t> prices(Side side) const {
    std::map<std::uint32_t, int> seen;
    for (const auto& [ref, o] : orders_) {
      if (o.side == side) seen[o.price] = 1;
    }
    std::vector<std::uint32_t> out;
    for (const auto& [p, unused] : seen) out.push_back(p);
    if (side == Side::Buy) std::reverse(out.begin(), out.end());
    return out;
  }

  [[nodiscard]] std::optional<md::LevelView> level(Side side, std::uint32_t price) const {
    md::LevelView v{price, 0, 0};
    for (const auto& [ref, o] : orders_) {
      if (o.side == side && o.price == price) {
        v.shares += o.shares;
        ++v.orders;
      }
    }
    if (v.orders == 0) return std::nullopt;
    return v;
  }

  [[nodiscard]] std::optional<md::LevelView> best(Side side) const {
    const auto p = prices(side);
    if (p.empty()) return std::nullopt;
    return level(side, p.front());
  }

  /// Orders at a level in arrival order.
  [[nodiscard]] std::vector<std::pair<std::uint64_t, std::uint32_t>> queue(Side side, std::uint32_t price) const {
    std::vector<std::pair<std::uint64_t, std::pair<std::uint64_t, std::uint32_t>>> tmp;
    for (const auto& [ref, o] : orders_) {
      if (o.side == side && o.price == price) tmp.push_back({o.arrival, {ref, o.shares}});
    }
    std::sort(tmp.begin(), tmp.end());
    std::vector<std::pair<std::uint64_t, std::uint32_t>> out;
    for (const auto& t : tmp) out.push_back(t.second);
    return out;
  }

  [[nodiscard]] std::uint64_t shares_ahead(std::uint64_t ref) const {
    auto it = orders_.find(ref);
    if (it == orders_.end()) return 0;
    std::uint64_t ahead = 0;
    for (const auto& [r, o] : orders_) {
      if (o.side == it->second.side && o.price == it->second.price && o.arrival < it->second.arrival) {
        ahead += o.shares;
      }
    }
    return ahead;
  }

  /// Same canonical encoding as md::Book::state_hash(), computed from scratch.
  [[nodiscard]] std::uint64_t state_hash() const {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    auto mix = [&h](std::uint64_t v, int bytes) {
      for (int i = 0; i < bytes; ++i) {
        h ^= (v >> (8 * i)) & 0xffU;
        h *= 0x100000001b3ULL;
      }
    };
    for (const Side s : {Side::Buy, Side::Sell}) {
      const auto ps = prices(s);
      mix(ps.size(), 8);
      for (const std::uint32_t p : ps) {
        const auto lv = *level(s, p);
        mix(p, 4);
        mix(lv.shares, 8);
        mix(lv.orders, 4);
        for (const auto& [ref, shares] : queue(s, p)) {
          mix(ref, 8);
          mix(shares, 4);
        }
      }
    }
    return h;
  }

 private:
  struct Order {
    Side side;
    std::uint32_t price;
    std::uint32_t shares;
    std::uint64_t arrival;
  };

  Status check_new(Side side, std::uint32_t shares, std::uint32_t price,
                   std::optional<std::uint64_t> replacing) const {
    if (shares == 0) return Status::ZeroShares;
    if (price == 0) return Status::ZeroPrice;
    const Side opp = side == Side::Buy ? Side::Sell : Side::Buy;
    if (const auto ob = best(opp)) {
      if (price == ob->price) return Status::WouldLock;
      if (side == Side::Buy ? price > ob->price : price < ob->price) return Status::WouldCross;
    }
    // Level capacity counted on the book as it would be after removing the
    // replaced order.
    std::map<std::uint32_t, int> lv;
    for (const auto& [ref, o] : orders_) {
      if (o.side == side && (!replacing || ref != *replacing)) lv[o.price] = 1;
    }
    if (lv.count(price) == 0 && lv.size() >= max_levels_) return Status::LevelCapacity;
    return Status::Applied;
  }

  std::size_t max_orders_;
  std::size_t max_levels_;
  std::map<std::uint64_t, Order> orders_;
  std::uint64_t next_arrival_{0};
};

}  // namespace lob::testkit
