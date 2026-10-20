#pragma once

// EventAuditor: checks the engine's event stream against the properties in
// DESIGN.md §3.6 (A1–A6), using only the commands and the events.
//
// It keeps its own reconstruction of the resting book (a flat vector, linear
// scans) and derives from it what every event must look like: which maker a
// trade must hit (price-time priority), at what price, for how much, and
// what each remaining quantity must be. It does not implement matching
// itself; it only checks that each event is the one the rules allow.
//
// Usage: for every processed command, call audit(command, events_of_command),
// then optionally matches(engine.book()).

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "engine_test_util.hpp"
#include "lob/core/validation.hpp"
#include "lob/engine/events.hpp"

namespace lob::test {

class EventAuditor {
 public:
  explicit EventAuditor(const EngineConfig& config) : config_(config) {}

  ::testing::AssertionResult audit(const Command& command, std::span<const Event> events) {
    events_ = events;
    next_ = 0;
    auto result = std::visit([&](const auto& c) { return audit_command(c); }, command);
    if (result && next_ != events_.size()) {
      return fail("unexpected extra event " + describe(events_[next_]));
    }
    if (result) {
      result = check_conservation();
    }
    return result;
  }

  /// A6: the reconstructed book equals the engine's book, order by order;
  /// and A5 stated against the ENGINE's book: total accepted quantity =
  /// 2 * traded + quantity resting in the engine + cancelled. (The per-command
  /// check in audit() uses the auditor's own reconstruction, where it holds
  /// by construction once every event has been validated; this one is the
  /// engine-facing statement.)
  ::testing::AssertionResult matches(const OrderBook& book) const {
    std::int64_t engine_resting = 0;
    for (const Side side : {Side::Buy, Side::Sell}) {
      book.for_each_order(side, [&](Price, const RestingOrder& o) { engine_resting += o.remaining; });
    }
    if (accepted_ != 2 * traded_ + engine_resting + cancelled_) {
      return ::testing::AssertionFailure()
             << "conservation vs engine book: accepted " << accepted_ << " != 2*traded " << traded_
             << " + engine resting " << engine_resting << " + cancelled " << cancelled_;
    }
    for (const Side side : {Side::Buy, Side::Sell}) {
      std::vector<Resting> actual;
      book.for_each_order(side, [&](Price price, const RestingOrder& o) {
        actual.push_back(Resting{o.id, side, price, 0, o.remaining, o.seq});
      });
      std::vector<Resting> expected = sorted(side);
      if (actual.size() != expected.size()) {
        return ::testing::AssertionFailure()
               << to_string(side) << ": book has " << actual.size() << " orders, audit expects "
               << expected.size();
      }
      for (std::size_t i = 0; i < actual.size(); ++i) {
        const Resting& a = actual[i];
        const Resting& e = expected[i];
        if (a.id != e.id || a.price != e.price || a.remaining != e.remaining || a.seq != e.seq) {
          return ::testing::AssertionFailure()
                 << to_string(side) << " position " << i << ": book has id " << a.id << " @"
                 << a.price << " x" << a.remaining << " #" << a.seq << ", audit expects id " << e.id
                 << " @" << e.price << " x" << e.remaining << " #" << e.seq;
        }
      }
    }
    return ::testing::AssertionSuccess();
  }

  [[nodiscard]] std::int64_t accepted_quantity() const { return accepted_; }
  [[nodiscard]] std::int64_t traded_quantity() const { return traded_; }
  [[nodiscard]] std::int64_t cancelled_quantity() const { return cancelled_; }

 private:
  struct Resting {
    OrderId id;
    Side side;
    Price price;
    Quantity original;  // 0 when unknown (comparison copies)
    Quantity remaining;
    SeqNum seq;
  };

  // ------------------------------------------------------------- commands

  ::testing::AssertionResult audit_command(const NewOrder& order) {
    const Event* first = take();
    if (first == nullptr) {
      return fail("new order produced no event");
    }
    if (const auto* rejected = std::get_if<OrderRejected>(first)) {
      return audit_new_order_rejection(order, *rejected);
    }
    const auto* accepted = std::get_if<OrderAccepted>(first);
    if (accepted == nullptr) {
      return fail("new order must start with ACCEPTED or REJECTED, got " + describe(*first));
    }
    if (auto ok = check_seq(*first); !ok) {
      return ok;
    }
    if (validate_new_order(order, config_) || find(order.id)) {
      return fail("accepted an order that must be rejected: " + describe(*first));
    }
    if (accepted->id != order.id || accepted->side != order.side ||
        accepted->price != order.price || accepted->quantity != order.quantity) {
      return fail("ACCEPTED does not match the command: " + describe(*first));
    }
    accepted_ += order.quantity;

    Quantity remaining = order.quantity;
    while (remaining > 0) {
      const Event* e = peek();
      if (e == nullptr || !std::holds_alternative<TradeExecuted>(*e)) {
        break;
      }
      take();
      if (auto ok = audit_trade(order, std::get<TradeExecuted>(*e), remaining); !ok) {
        return ok;
      }
    }
    if (remaining == 0) {
      return ::testing::AssertionSuccess();  // fully filled: nothing may follow
    }

    // A4: the remainder may only stop trading if no crossing maker is left.
    if (const Resting* best = best_on(opposite(order.side));
        best != nullptr && crosses(order.side, order.price, best->price)) {
      return fail("order " + std::to_string(order.id) + " stopped trading with crossing maker " +
                  std::to_string(best->id) + " still resting");
    }
    const Event* last = take();
    if (last == nullptr) {
      return fail("order " + std::to_string(order.id) + " has remainder " +
                  std::to_string(remaining) + " but no RESTED/CANCELLED event");
    }
    if (auto ok = check_seq(*last); !ok) {
      return ok;
    }
    if (const auto* rested = std::get_if<OrderRested>(last)) {
      if (rested->id != order.id || rested->side != order.side || rested->price != order.price ||
          rested->remaining != remaining) {
        return fail("RESTED does not match the remainder: " + describe(*last));
      }
      resting_.push_back(Resting{order.id, order.side, order.price, order.quantity, remaining,
                                 accepted->seq});  // priority = acceptance seq
      return ::testing::AssertionSuccess();
    }
    if (const auto* cancelled = std::get_if<OrderCancelled>(last);
        cancelled != nullptr && cancelled->reason == CancelReason::InsufficientResources) {
      if (cancelled->id != order.id || cancelled->remaining != remaining) {
        return fail("resource CANCELLED does not match the remainder: " + describe(*last));
      }
      cancelled_ += remaining;
      return ::testing::AssertionSuccess();
    }
    return fail("remainder must be RESTED or CANCELLED(INSUFFICIENT_RESOURCES), got " +
                describe(*last));
  }

  ::testing::AssertionResult audit_command(const CancelOrder& cancel) {
    const Event* e = take();
    if (e == nullptr) {
      return fail("cancel produced no event");
    }
    if (auto ok = check_seq(*e); !ok) {
      return ok;
    }
    const auto it = position(cancel.id);
    if (const auto* rejected = std::get_if<OrderRejected>(e)) {
      const RejectReason expected =
          cancel.id == kInvalidOrderId ? RejectReason::InvalidOrderId : RejectReason::UnknownOrderId;
      if (rejected->command != CommandType::CancelOrder || rejected->id != cancel.id ||
          rejected->reason != expected || it != resting_.end()) {
        return fail("wrong cancel rejection: " + describe(*e));
      }
      return ::testing::AssertionSuccess();
    }
    const auto* cancelled = std::get_if<OrderCancelled>(e);
    if (cancelled == nullptr || cancelled->reason != CancelReason::Requested) {
      return fail("cancel must produce CANCELLED(REQUESTED) or REJECTED, got " + describe(*e));
    }
    if (it == resting_.end()) {
      return fail("cancelled an order that is not resting: " + describe(*e));
    }
    if (cancelled->id != it->id || cancelled->side != it->side || cancelled->price != it->price ||
        cancelled->remaining != it->remaining) {
      return fail("CANCELLED does not match the resting order: " + describe(*e));
    }
    cancelled_ += it->remaining;
    resting_.erase(it);
    return ::testing::AssertionSuccess();
  }

  ::testing::AssertionResult audit_new_order_rejection(const NewOrder& order,
                                                       const OrderRejected& rejected) {
    if (auto ok = check_seq(Event{rejected}); !ok) {
      return ok;
    }
    if (rejected.command != CommandType::NewOrder || rejected.id != order.id) {
      return fail("REJECTED does not match the command: " + describe(Event{rejected}));
    }
    std::optional<RejectReason> expected = validate_new_order(order, config_);
    if (!expected && find(order.id)) {
      expected = RejectReason::DuplicateOrderId;
    }
    // BOOK_FULL legitimacy depends on the match plan; the differential test
    // checks it against the reference engine. Here: only BOOK_FULL may remain.
    if (!expected) {
      expected = RejectReason::BookFull;
    }
    if (rejected.reason != *expected) {
      return fail("rejection reason " + std::string(to_string(rejected.reason)) + ", expected " +
                  std::string(to_string(*expected)));
    }
    return ::testing::AssertionSuccess();
  }

  // A3: the trade must hit the best maker, at its price, for min(remaining).
  ::testing::AssertionResult audit_trade(const NewOrder& order, const TradeExecuted& trade,
                                         Quantity& remaining) {
    if (auto ok = check_seq(Event{trade}); !ok) {
      return ok;
    }
    Resting* maker = best_on(opposite(order.side));
    if (maker == nullptr) {
      return fail("trade with an empty opposite side: " + describe(Event{trade}));
    }
    if (trade.taker_id != order.id || trade.taker_side != order.side) {
      return fail("trade taker does not match the order: " + describe(Event{trade}));
    }
    if (trade.maker_id != maker->id) {
      return fail("price-time priority violated: traded with " + std::to_string(trade.maker_id) +
                  ", best maker is " + std::to_string(maker->id));
    }
    if (trade.price != maker->price) {
      return fail("trade price " + std::to_string(trade.price) + " != maker price " +
                  std::to_string(maker->price));
    }
    if (!crosses(order.side, order.price, trade.price)) {
      return fail("trade at " + std::to_string(trade.price) + " violates taker limit " +
                  std::to_string(order.price));
    }
    const Quantity expected_quantity = std::min(remaining, maker->remaining);
    if (trade.quantity != expected_quantity) {
      return fail("trade quantity " + std::to_string(trade.quantity) + ", expected " +
                  std::to_string(expected_quantity));
    }
    remaining -= trade.quantity;
    maker->remaining -= trade.quantity;
    traded_ += trade.quantity;
    if (trade.taker_remaining != remaining || trade.maker_remaining != maker->remaining) {
      return fail("trade remaining quantities wrong: " + describe(Event{trade}));
    }
    if (maker->remaining == 0) {
      resting_.erase(resting_.begin() + (maker - resting_.data()));
    }
    return ::testing::AssertionSuccess();
  }

  // ------------------------------------------------------------- checks

  // A1: seq starts at 1 and increases by exactly 1.
  ::testing::AssertionResult check_seq(const Event& e) {
    if (seq_of(e) != expected_seq_) {
      return fail("seq " + std::to_string(seq_of(e)) + ", expected " +
                  std::to_string(expected_seq_) + " in " + describe(e));
    }
    ++expected_seq_;
    return ::testing::AssertionSuccess();
  }

  // A5: per-order and global quantity conservation.
  ::testing::AssertionResult check_conservation() const {
    std::int64_t resting_total = 0;
    for (const Resting& r : resting_) {
      if (r.remaining <= 0 || r.remaining > r.original) {
        return fail("order " + std::to_string(r.id) + " remaining " + std::to_string(r.remaining) +
                    " outside (0, original]");
      }
      resting_total += r.remaining;
    }
    if (accepted_ != 2 * traded_ + resting_total + cancelled_) {
      return fail("conservation: accepted " + std::to_string(accepted_) + " != 2*traded " +
                  std::to_string(traded_) + " + resting " + std::to_string(resting_total) +
                  " + cancelled " + std::to_string(cancelled_));
    }
    return ::testing::AssertionSuccess();
  }

  // ------------------------------------------------------------- helpers

  static bool crosses(Side taker_side, Price limit, Price maker_price) {
    return taker_side == Side::Buy ? maker_price <= limit : maker_price >= limit;
  }

  Resting* best_on(Side side) {
    Resting* best = nullptr;
    for (Resting& r : resting_) {
      if (r.side != side) {
        continue;
      }
      const bool better = best == nullptr ||
                          (r.price != best->price
                               ? (side == Side::Buy ? r.price > best->price : r.price < best->price)
                               : r.seq < best->seq);
      if (better) {
        best = &r;
      }
    }
    return best;
  }

  std::vector<Resting> sorted(Side side) const {
    std::vector<Resting> out;
    for (const Resting& r : resting_) {
      if (r.side == side) {
        out.push_back(r);
      }
    }
    std::sort(out.begin(), out.end(), [side](const Resting& a, const Resting& b) {
      if (a.price != b.price) {
        return side == Side::Buy ? a.price > b.price : a.price < b.price;
      }
      return a.seq < b.seq;
    });
    return out;
  }

  std::vector<Resting>::iterator position(OrderId id) {
    return std::find_if(resting_.begin(), resting_.end(), [id](const Resting& r) { return r.id == id; });
  }
  const Resting* find(OrderId id) const {
    for (const Resting& r : resting_) {
      if (r.id == id) {
        return &r;
      }
    }
    return nullptr;
  }

  const Event* peek() const { return next_ < events_.size() ? &events_[next_] : nullptr; }
  const Event* take() { return next_ < events_.size() ? &events_[next_++] : nullptr; }

  static ::testing::AssertionResult fail(const std::string& message) {
    return ::testing::AssertionFailure() << message;
  }

  EngineConfig config_;
  std::vector<Resting> resting_;
  SeqNum expected_seq_{1};
  std::int64_t accepted_{0};
  std::int64_t traded_{0};
  std::int64_t cancelled_{0};
  std::span<const Event> events_;
  std::size_t next_{0};
};

}  // namespace lob::test
