#include "lob/engine/matching_engine.hpp"

#include <algorithm>
#include <new>
#include <stdexcept>
#include <variant>

#include "lob/core/checked.hpp"
#include "lob/core/validation.hpp"

namespace lob {

namespace {

/// True if a taker on `taker_side` with limit `limit` may trade with a maker
/// resting at `maker_price`. Equal prices trade.
[[nodiscard]] constexpr bool crosses(Side taker_side, Price limit, Price maker_price) noexcept {
  return taker_side == Side::Buy ? maker_price <= limit : maker_price >= limit;
}

/// Guarantees capacity for `additional` more events, growing geometrically:
/// reserving exactly size()+additional on every command would reallocate on
/// every command for a caller that never clears the vector (O(N^2) copying).
/// Called in the prepare phase only; may throw, but never changes contents.
void ensure_capacity(std::vector<Event>& events, std::size_t additional) {
  const std::size_t needed = events.size() + additional;
  if (needed <= events.capacity()) {
    return;
  }
  const std::size_t doubled = std::min(events.capacity() * 2, events.max_size());
  events.reserve(std::max(needed, doubled));
}

}  // namespace

MatchingEngine::MatchingEngine(const EngineConfig& config) : book_(config) {}

// ----------------------------------------------------------- snapshot / restore

EngineSnapshot MatchingEngine::snapshot() const {
  EngineSnapshot s{.config = config(), .last_seq = last_seq_, .orders = {}};
  s.orders.reserve(book_.order_count());
  for (const Side side : {Side::Buy, Side::Sell}) {  // canonical: bids, then asks
    book_.for_each_order(side, [&](Price price, const RestingOrder& o) {
      s.orders.push_back(
          SnapshotOrder{.id = o.id, .side = side, .price = price, .remaining = o.remaining, .seq = o.seq});
    });
  }
  return s;
}

MatchingEngine MatchingEngine::restore(const EngineSnapshot& snapshot) {
  if (const auto violation = find_snapshot_violation(snapshot)) {
    throw std::invalid_argument("MatchingEngine::restore: invalid snapshot: " + *violation);
  }
  MatchingEngine engine{snapshot.config};
  // Re-adding the orders in ascending seq order satisfies every
  // OrderBook::add precondition (validated fields, unique ids, capacity,
  // strictly increasing seq, and no crossing: any subset of a non-crossed
  // book is non-crossed) and rebuilds each FIFO queue in its original order,
  // because FIFO order within a level is seq order.
  std::vector<SnapshotOrder> by_seq = snapshot.orders;
  std::sort(by_seq.begin(), by_seq.end(),
            [](const SnapshotOrder& a, const SnapshotOrder& b) { return a.seq < b.seq; });
  for (const SnapshotOrder& o : by_seq) {
    engine.book_.add(NewOrder{.id = o.id, .side = o.side, .price = o.price, .quantity = o.remaining},
                     o.seq);
  }
  engine.last_seq_ = snapshot.last_seq;
  return engine;
}

void MatchingEngine::process(const Command& command, std::vector<Event>& events) {
  std::visit([&](const auto& c) { process(c, events); }, command);
}

// ----------------------------------------------------------------- new order

void MatchingEngine::process(const NewOrder& order, std::vector<Event>& events) {
  // ---- prepare: read-only ----------------------------------------------
  std::optional<RejectReason> reason = validate_new_order(order, config());
  if (!reason && book_.contains(order.id)) {
    reason = RejectReason::DuplicateOrderId;
  }
  MatchPlan plan;
  if (!reason) {
    plan = plan_match(order);
    // Only an order that will actually rest can be refused for capacity, and
    // the makers it fills completely free their slots first (D-021).
    const bool rests = plan.fill_quantity < order.quantity;
    const std::size_t resting_after_fills = book_.order_count() - plan.makers_filled;
    if (rests && resting_after_fills >= static_cast<std::size_t>(config().max_live_orders)) {
      reason = RejectReason::BookFull;
    }
  }
  if (reason) {
    reject(CommandType::NewOrder, order.id, *reason, events);
    return;
  }

  const Quantity residual = order.quantity - plan.fill_quantity;
  const std::size_t event_count = 1 + plan.trades + (residual > 0 ? 1 : 0);
  SeqNum seq = first_seq_of_block(event_count);
  ensure_capacity(events, event_count);

  // ---- commit: events fit in the reserved capacity; fills do not allocate
  const SeqNum accepted_seq = seq;
  events.push_back(OrderAccepted{
      .seq = seq, .id = order.id, .side = order.side, .price = order.price, .quantity = order.quantity});
  last_seq_ = seq;

  const Side maker_side = opposite(order.side);
  Quantity remaining = order.quantity;
  std::size_t trades = 0;
  while (remaining > 0) {
    const std::optional<OrderView> maker = book_.front(maker_side);
    if (!maker || !crosses(order.side, order.price, maker->price)) {
      break;
    }
    const Quantity quantity = std::min(remaining, maker->remaining);
    const Fill fill = book_.fill_front(maker_side, quantity);
    remaining -= quantity;
    events.push_back(TradeExecuted{.seq = ++seq,
                                   .taker_id = order.id,
                                   .maker_id = fill.maker_id,
                                   .taker_side = order.side,
                                   .price = fill.price,
                                   .quantity = fill.quantity,
                                   .taker_remaining = remaining,
                                   .maker_remaining = fill.maker_remaining});
    last_seq_ = seq;
    ++trades;
  }
  if (trades != plan.trades || remaining != residual) {
    // The book did not change between planning and execution, so this can
    // only be a bug. Reported loudly; the events so far describe the state.
    throw std::logic_error("MatchingEngine: execution diverged from the match plan");
  }
  if (remaining == 0) {
    return;
  }

  // Preconditions of OrderBook::add hold by construction: the order passed
  // stateless validation, its id is not resting, capacity was checked
  // against the post-fill count, accepted_seq exceeds every earlier seq, and
  // the loop above consumed all crossing liquidity. So add can only fail
  // with std::bad_alloc, and then leaves the book unchanged (M2 strong
  // guarantee).
  ++seq;
  try {
    book_.add(NewOrder{.id = order.id, .side = order.side, .price = order.price, .quantity = remaining},
              accepted_seq);
  } catch (const std::bad_alloc&) {
    events.push_back(OrderCancelled{.seq = seq,
                                    .id = order.id,
                                    .side = order.side,
                                    .price = order.price,
                                    .remaining = remaining,
                                    .reason = CancelReason::InsufficientResources});
    last_seq_ = seq;
    throw;
  }
  events.push_back(OrderRested{
      .seq = seq, .id = order.id, .side = order.side, .price = order.price, .remaining = remaining});
  last_seq_ = seq;
}

MatchingEngine::MatchPlan MatchingEngine::plan_match(const NewOrder& order) const {
  MatchPlan plan;
  Quantity remaining = order.quantity;
  book_.for_each_order_while(opposite(order.side), [&](Price price, const RestingOrder& maker) {
    if (!crosses(order.side, order.price, price)) {
      return false;  // priority order: no later maker can cross either
    }
    const Quantity quantity = std::min(remaining, maker.remaining);
    remaining -= quantity;
    plan.fill_quantity += quantity;
    ++plan.trades;
    if (quantity == maker.remaining) {
      ++plan.makers_filled;
    }
    return remaining > 0;
  });
  return plan;
}

// -------------------------------------------------------------------- cancel

void MatchingEngine::process(const CancelOrder& cancel, std::vector<Event>& events) {
  std::optional<RejectReason> reason = validate_cancel_order(cancel);
  if (!reason && !book_.contains(cancel.id)) {
    reason = RejectReason::UnknownOrderId;
  }
  if (reason) {
    reject(CommandType::CancelOrder, cancel.id, *reason, events);
    return;
  }

  const SeqNum seq = first_seq_of_block(1);
  ensure_capacity(events, 1);
  // ---- commit: cancel is noexcept and the event fits.
  const std::optional<CancelledOrder> cancelled = book_.cancel(cancel.id);
  events.push_back(OrderCancelled{.seq = seq,
                                  .id = cancelled->id,
                                  .side = cancelled->side,
                                  .price = cancelled->price,
                                  .remaining = cancelled->remaining,
                                  .reason = CancelReason::Requested});
  last_seq_ = seq;
}

// ------------------------------------------------------------------- helpers

void MatchingEngine::reject(CommandType command, OrderId id, RejectReason reason,
                            std::vector<Event>& events) {
  const SeqNum seq = first_seq_of_block(1);
  ensure_capacity(events, 1);
  events.push_back(OrderRejected{.seq = seq, .command = command, .id = id, .reason = reason});
  last_seq_ = seq;
}

SeqNum MatchingEngine::first_seq_of_block(std::size_t count) const {
  if (!checked_add<SeqNum>(last_seq_, static_cast<SeqNum>(count))) {
    throw std::overflow_error("MatchingEngine: event sequence number would overflow");
  }
  return last_seq_ + 1;
}

std::optional<std::string> MatchingEngine::find_invariant_violation() const {
  if (auto violation = book_.find_invariant_violation()) {
    return violation;
  }
  std::optional<std::string> result;
  for (const Side side : {Side::Buy, Side::Sell}) {
    book_.for_each_order_while(side, [&](Price, const RestingOrder& order) {
      if (order.seq > last_seq_) {
        result = "order " + std::to_string(order.id) + " has seq " + std::to_string(order.seq) +
                 " > engine last_seq " + std::to_string(last_seq_);
        return false;
      }
      return true;
    });
    if (result) {
      return result;
    }
  }
  return std::nullopt;
}

}  // namespace lob
