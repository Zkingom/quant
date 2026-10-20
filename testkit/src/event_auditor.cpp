#include "lob/testkit/event_auditor.hpp"

#include <algorithm>
#include <variant>

#include "lob/core/validation.hpp"
#include "lob/testkit/describe.hpp"

namespace lob::testkit {

namespace {

bool crosses(Side taker_side, Price limit, Price maker_price) {
  return taker_side == Side::Buy ? maker_price <= limit : maker_price >= limit;
}

// int64_t / uint64_t / size_t are long / unsigned long on the supported
// (LP64 Linux) targets.
std::string str(long v) { return std::to_string(v); }
std::string str(unsigned long v) { return std::to_string(v); }

}  // namespace

std::string to_string(Int128 value) {
  if (value == 0) {
    return "0";
  }
  const bool negative = value < 0;
  std::string digits;
  while (value != 0) {
    const auto digit = static_cast<int>(value % 10);
    digits.push_back(static_cast<char>('0' + (negative ? -digit : digit)));
    value /= 10;
  }
  if (negative) {
    digits.push_back('-');
  }
  std::reverse(digits.begin(), digits.end());
  return digits;
}

// --------------------------------------------------------------- entry points

EventAuditor::Violation EventAuditor::audit(const Command& command, std::span<const Event> events) {
  events_ = events;
  next_ = 0;
  Violation result = std::visit(
      [&](const auto& c) -> Violation {
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, NewOrder>) {
          return audit_new(c);
        } else {
          return audit_cancel(c);
        }
      },
      command);
  if (!result && next_ != events_.size()) {
    return "unexpected extra event " + describe(events_[next_]);
  }
  if (!result) {
    result = check_conservation();
  }
  return result;
}

EventAuditor::Violation EventAuditor::matches(const OrderBook& book) const {
  Int128 engine_resting = 0;
  for (const Side side : {Side::Buy, Side::Sell}) {
    book.for_each_order(side, [&](Price, const RestingOrder& o) { engine_resting += o.remaining; });
  }
  if (accepted_ != 2 * traded_ + engine_resting + cancelled_) {
    return "conservation vs engine book: accepted " + to_string(accepted_) + " != 2*traded " +
           to_string(traded_) + " + engine resting " + to_string(engine_resting) +
           " + cancelled " + to_string(cancelled_);
  }
  for (const Side side : {Side::Buy, Side::Sell}) {
    std::vector<Resting> actual;
    book.for_each_order(side, [&](Price price, const RestingOrder& o) {
      actual.push_back(Resting{o.id, side, price, 0, o.remaining, o.seq});
    });
    const std::vector<Resting> expected = sorted(side);
    if (actual.size() != expected.size()) {
      return std::string(lob::to_string(side)) + ": book has " + str(actual.size()) +
             " orders, audit expects " + str(expected.size());
    }
    for (std::size_t i = 0; i < actual.size(); ++i) {
      const Resting& a = actual[i];
      const Resting& e = expected[i];
      if (a.id != e.id || a.price != e.price || a.remaining != e.remaining || a.seq != e.seq) {
        return std::string(lob::to_string(side)) + " position " + str(i) + ": book has id " +
               str(a.id) + " @" + str(a.price) + " x" + str(a.remaining) + " #" + str(a.seq) +
               ", audit expects id " + str(e.id) + " @" + str(e.price) + " x" + str(e.remaining) +
               " #" + str(e.seq);
      }
    }
  }
  return std::nullopt;
}

// ------------------------------------------------------------------ commands

EventAuditor::Violation EventAuditor::audit_new(const NewOrder& order) {
  const Event* first = take();
  if (first == nullptr) {
    return "new order produced no event";
  }
  if (const auto* rejected = std::get_if<OrderRejected>(first)) {
    return audit_new_rejection(order, *rejected);
  }
  const auto* accepted = std::get_if<OrderAccepted>(first);
  if (accepted == nullptr) {
    return "new order must start with ACCEPTED or REJECTED, got " + describe(*first);
  }
  if (auto v = check_seq(*first)) {
    return v;
  }
  if (validate_new_order(order, config_) || find(order.id)) {
    return "accepted an order that must be rejected: " + describe(*first);
  }
  if (accepted->id != order.id || accepted->side != order.side ||
      accepted->price != order.price || accepted->quantity != order.quantity) {
    return "ACCEPTED does not match the command: " + describe(*first);
  }
  accepted_ += order.quantity;

  Quantity remaining = order.quantity;
  while (remaining > 0) {
    const Event* e = peek();
    if (e == nullptr || !std::holds_alternative<TradeExecuted>(*e)) {
      break;
    }
    take();
    if (auto v = audit_trade(order, std::get<TradeExecuted>(*e), remaining)) {
      return v;
    }
  }
  if (remaining == 0) {
    return std::nullopt;  // fully filled: nothing may follow
  }

  // A4: the remainder may only stop trading if no crossing maker is left.
  if (const Resting* best = best_on(opposite(order.side));
      best != nullptr && crosses(order.side, order.price, best->price)) {
    return "order " + str(order.id) + " stopped trading with crossing maker " + str(best->id) +
           " still resting";
  }
  const Event* last = take();
  if (last == nullptr) {
    return "order " + str(order.id) + " has remainder " + str(remaining) +
           " but no RESTED/CANCELLED event";
  }
  if (auto v = check_seq(*last)) {
    return v;
  }
  if (const auto* rested = std::get_if<OrderRested>(last)) {
    if (rested->id != order.id || rested->side != order.side || rested->price != order.price ||
        rested->remaining != remaining) {
      return "RESTED does not match the remainder: " + describe(*last);
    }
    resting_.push_back(
        Resting{order.id, order.side, order.price, order.quantity, remaining, accepted->seq});
    return std::nullopt;
  }
  if (const auto* cancelled = std::get_if<OrderCancelled>(last);
      cancelled != nullptr && cancelled->reason == CancelReason::InsufficientResources) {
    if (cancelled->id != order.id || cancelled->remaining != remaining) {
      return "resource CANCELLED does not match the remainder: " + describe(*last);
    }
    cancelled_ += remaining;
    return std::nullopt;
  }
  return "remainder must be RESTED or CANCELLED(INSUFFICIENT_RESOURCES), got " + describe(*last);
}

EventAuditor::Violation EventAuditor::audit_cancel(const CancelOrder& cancel) {
  const Event* e = take();
  if (e == nullptr) {
    return "cancel produced no event";
  }
  if (auto v = check_seq(*e)) {
    return v;
  }
  const auto it = position(cancel.id);
  if (const auto* rejected = std::get_if<OrderRejected>(e)) {
    const RejectReason expected =
        cancel.id == kInvalidOrderId ? RejectReason::InvalidOrderId : RejectReason::UnknownOrderId;
    if (rejected->command != CommandType::CancelOrder || rejected->id != cancel.id ||
        rejected->reason != expected || it != resting_.end()) {
      return "wrong cancel rejection: " + describe(*e);
    }
    return std::nullopt;
  }
  const auto* cancelled = std::get_if<OrderCancelled>(e);
  if (cancelled == nullptr || cancelled->reason != CancelReason::Requested) {
    return "cancel must produce CANCELLED(REQUESTED) or REJECTED, got " + describe(*e);
  }
  if (it == resting_.end()) {
    return "cancelled an order that is not resting: " + describe(*e);
  }
  if (cancelled->id != it->id || cancelled->side != it->side || cancelled->price != it->price ||
      cancelled->remaining != it->remaining) {
    return "CANCELLED does not match the resting order: " + describe(*e);
  }
  cancelled_ += it->remaining;
  resting_.erase(it);
  return std::nullopt;
}

EventAuditor::Violation EventAuditor::audit_new_rejection(const NewOrder& order,
                                                          const OrderRejected& rejected) {
  if (auto v = check_seq(Event{rejected})) {
    return v;
  }
  if (rejected.command != CommandType::NewOrder || rejected.id != order.id) {
    return "REJECTED does not match the command: " + describe(Event{rejected});
  }
  std::optional<RejectReason> expected = validate_new_order(order, config_);
  if (!expected && find(order.id)) {
    expected = RejectReason::DuplicateOrderId;
  }
  // BOOK_FULL legitimacy depends on the match plan; differential tests check
  // it against the reference engine. Here: only BOOK_FULL may remain.
  if (!expected) {
    expected = RejectReason::BookFull;
  }
  if (rejected.reason != *expected) {
    return "rejection reason " + std::string(lob::to_string(rejected.reason)) + ", expected " +
           std::string(lob::to_string(*expected));
  }
  return std::nullopt;
}

// A3: the trade must hit the best maker, at its price, for min(remaining).
EventAuditor::Violation EventAuditor::audit_trade(const NewOrder& order, const TradeExecuted& trade,
                                                  Quantity& remaining) {
  if (auto v = check_seq(Event{trade})) {
    return v;
  }
  Resting* maker = best_on(opposite(order.side));
  if (maker == nullptr) {
    return "trade with an empty opposite side: " + describe(Event{trade});
  }
  if (trade.taker_id != order.id || trade.taker_side != order.side) {
    return "trade taker does not match the order: " + describe(Event{trade});
  }
  if (trade.maker_id != maker->id) {
    return "price-time priority violated: traded with " + str(trade.maker_id) +
           ", best maker is " + str(maker->id);
  }
  if (trade.price != maker->price) {
    return "trade price " + str(trade.price) + " != maker price " + str(maker->price);
  }
  if (!crosses(order.side, order.price, trade.price)) {
    return "trade at " + str(trade.price) + " violates taker limit " + str(order.price);
  }
  const Quantity expected_quantity = std::min(remaining, maker->remaining);
  if (trade.quantity != expected_quantity) {
    return "trade quantity " + str(trade.quantity) + ", expected " + str(expected_quantity);
  }
  remaining -= trade.quantity;
  maker->remaining -= trade.quantity;
  traded_ += trade.quantity;
  if (trade.taker_remaining != remaining || trade.maker_remaining != maker->remaining) {
    return "trade remaining quantities wrong: " + describe(Event{trade});
  }
  if (maker->remaining == 0) {
    resting_.erase(resting_.begin() + (maker - resting_.data()));
  }
  return std::nullopt;
}

// -------------------------------------------------------------------- checks

// A1: seq starts at 1 and increases by exactly 1.
EventAuditor::Violation EventAuditor::check_seq(const Event& e) {
  if (seq_of(e) != expected_seq_) {
    return "seq " + str(seq_of(e)) + ", expected " + str(expected_seq_) + " in " + describe(e);
  }
  ++expected_seq_;
  return std::nullopt;
}

// A5 on the auditor's own reconstruction (see header).
EventAuditor::Violation EventAuditor::check_conservation() const {
  Int128 resting_total = 0;
  for (const Resting& r : resting_) {
    if (r.remaining <= 0 || r.remaining > r.original) {
      return "order " + str(r.id) + " remaining " + str(r.remaining) + " outside (0, original]";
    }
    resting_total += r.remaining;
  }
  if (accepted_ != 2 * traded_ + resting_total + cancelled_) {
    return "conservation: accepted " + to_string(accepted_) + " != 2*traded " +
           to_string(traded_) + " + resting " + to_string(resting_total) + " + cancelled " +
           to_string(cancelled_);
  }
  return std::nullopt;
}

// ------------------------------------------------------------------- helpers

EventAuditor::Resting* EventAuditor::best_on(Side side) {
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

std::vector<EventAuditor::Resting> EventAuditor::sorted(Side side) const {
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

const EventAuditor::Resting* EventAuditor::find(OrderId id) const {
  for (const Resting& r : resting_) {
    if (r.id == id) {
      return &r;
    }
  }
  return nullptr;
}

std::vector<EventAuditor::Resting>::iterator EventAuditor::position(OrderId id) {
  return std::find_if(resting_.begin(), resting_.end(), [id](const Resting& r) { return r.id == id; });
}

const Event* EventAuditor::peek() const { return next_ < events_.size() ? &events_[next_] : nullptr; }
const Event* EventAuditor::take() { return next_ < events_.size() ? &events_[next_++] : nullptr; }

}  // namespace lob::testkit
