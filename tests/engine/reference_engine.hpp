#pragma once

// ReferenceEngine: an executable specification of MatchingEngine for
// differential testing. Shares only the value/event types and M1's
// stateless validate_new_order(); everything else is written differently
// from the real engine on purpose:
//   - the book is one flat vector of resting orders, no OrderBook;
//   - "best crossing maker" is a fresh linear scan before every fill;
//   - the match is simulated on a COPY of the book; BOOK_FULL is decided by
//     looking at the size of that copy, not by the engine's
//     "count - makers filled" formula; commit = assign the copy back.
// It has no exception handling: it never runs under fault injection.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

#include "lob/core/command.hpp"
#include "lob/core/config.hpp"
#include "lob/core/validation.hpp"
#include "lob/engine/events.hpp"

namespace lob::test {

class ReferenceEngine {
 public:
  struct Resting {
    OrderId id;
    Side side;
    Price price;
    Quantity remaining;
    SeqNum seq;

    friend bool operator==(const Resting&, const Resting&) = default;
  };

  explicit ReferenceEngine(const EngineConfig& config) : config_(config) {}

  std::vector<Event> process(const Command& command) {
    return std::visit([this](const auto& c) { return handle(c); }, command);
  }

  /// Resting orders of one side, best first (price, then seq).
  [[nodiscard]] std::vector<Resting> side_in_priority(Side side) const {
    std::vector<Resting> out;
    for (const Resting& r : book_) {
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

  [[nodiscard]] std::size_t order_count() const { return book_.size(); }
  [[nodiscard]] const std::vector<Resting>& orders() const { return book_; }

 private:
  std::vector<Event> handle(const NewOrder& order) {
    if (const auto reason = validate_new_order(order, config_)) {
      return {reject(CommandType::NewOrder, order.id, *reason)};
    }
    for (const Resting& r : book_) {
      if (r.id == order.id) {
        return {reject(CommandType::NewOrder, order.id, RejectReason::DuplicateOrderId)};
      }
    }

    // Simulate on a copy; nothing below touches book_ until the commit.
    std::vector<Resting> book = book_;
    struct Execution {
      OrderId maker;
      Price price;
      Quantity quantity;
      Quantity taker_remaining;
      Quantity maker_remaining;
    };
    std::vector<Execution> executions;
    Quantity remaining = order.quantity;
    while (remaining > 0) {
      std::size_t best = book.size();
      for (std::size_t i = 0; i < book.size(); ++i) {
        const Resting& r = book[i];
        if (r.side == order.side) {
          continue;
        }
        const bool tradable = order.side == Side::Buy ? r.price <= order.price : r.price >= order.price;
        if (!tradable) {
          continue;
        }
        if (best == book.size()) {
          best = i;
          continue;
        }
        const Resting& b = book[best];
        const bool better_price = order.side == Side::Buy ? r.price < b.price : r.price > b.price;
        if (better_price || (r.price == b.price && r.seq < b.seq)) {
          best = i;
        }
      }
      if (best == book.size()) {
        break;
      }
      Resting& maker = book[best];
      const Quantity q = std::min(remaining, maker.remaining);
      remaining -= q;
      maker.remaining -= q;
      executions.push_back(Execution{maker.id, maker.price, q, remaining, maker.remaining});
      if (maker.remaining == 0) {
        book.erase(book.begin() + static_cast<std::ptrdiff_t>(best));
      }
    }

    if (remaining > 0 && static_cast<std::int64_t>(book.size()) >= config_.max_live_orders) {
      return {reject(CommandType::NewOrder, order.id, RejectReason::BookFull)};  // copy discarded
    }

    // Commit.
    book_ = std::move(book);
    std::vector<Event> events;
    const SeqNum accepted_seq = ++seq_;
    events.push_back(OrderAccepted{.seq = accepted_seq,
                                   .id = order.id,
                                   .side = order.side,
                                   .price = order.price,
                                   .quantity = order.quantity});
    for (const Execution& x : executions) {
      events.push_back(TradeExecuted{.seq = ++seq_,
                                     .taker_id = order.id,
                                     .maker_id = x.maker,
                                     .taker_side = order.side,
                                     .price = x.price,
                                     .quantity = x.quantity,
                                     .taker_remaining = x.taker_remaining,
                                     .maker_remaining = x.maker_remaining});
    }
    if (remaining > 0) {
      book_.push_back(Resting{order.id, order.side, order.price, remaining, accepted_seq});
      events.push_back(OrderRested{.seq = ++seq_,
                                   .id = order.id,
                                   .side = order.side,
                                   .price = order.price,
                                   .remaining = remaining});
    }
    return events;
  }

  std::vector<Event> handle(const CancelOrder& cancel) {
    if (cancel.id == kInvalidOrderId) {
      return {reject(CommandType::CancelOrder, cancel.id, RejectReason::InvalidOrderId)};
    }
    for (auto it = book_.begin(); it != book_.end(); ++it) {
      if (it->id == cancel.id) {
        const Resting r = *it;
        book_.erase(it);
        return {OrderCancelled{.seq = ++seq_,
                               .id = r.id,
                               .side = r.side,
                               .price = r.price,
                               .remaining = r.remaining,
                               .reason = CancelReason::Requested}};
      }
    }
    return {reject(CommandType::CancelOrder, cancel.id, RejectReason::UnknownOrderId)};
  }

  Event reject(CommandType command, OrderId id, RejectReason reason) {
    return OrderRejected{.seq = ++seq_, .command = command, .id = id, .reason = reason};
  }

  EngineConfig config_;
  std::vector<Resting> book_;
  SeqNum seq_{0};
};

}  // namespace lob::test
