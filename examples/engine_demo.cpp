// Demonstrates M3: a small scripted session through the matching engine.
// Every command is printed with the events it produced, followed by the
// book. Covers resting, a multi-level sweep at maker prices, a partial fill
// whose remainder rests, cancels, and each kind of rejection.

#include <cstdio>
#include <limits>
#include <string>
#include <variant>
#include <vector>

#include "lob/engine/matching_engine.hpp"

namespace {

using lob::Side;

std::string describe(const lob::Command& command) {
  if (const auto* o = std::get_if<lob::NewOrder>(&command)) {
    return "NEW    id=" + std::to_string(o->id) + ' ' + std::string(lob::to_string(o->side)) + ' ' +
           std::to_string(o->price) + 'x' + std::to_string(o->quantity);
  }
  return "CANCEL id=" + std::to_string(std::get<lob::CancelOrder>(command).id);
}

std::string describe(const lob::Event& event) {
  struct Visitor {
    std::string operator()(const lob::OrderAccepted& e) const {
      return "ACCEPTED  id=" + std::to_string(e.id);
    }
    std::string operator()(const lob::OrderRejected& e) const {
      return "REJECTED  id=" + std::to_string(e.id) + ' ' + std::string(lob::to_string(e.reason));
    }
    std::string operator()(const lob::TradeExecuted& e) const {
      return "TRADE     taker=" + std::to_string(e.taker_id) + " maker=" + std::to_string(e.maker_id) +
             ' ' + std::to_string(e.quantity) + " @ " + std::to_string(e.price) +
             "  (taker left " + std::to_string(e.taker_remaining) + ", maker left " +
             std::to_string(e.maker_remaining) + ')';
    }
    std::string operator()(const lob::OrderRested& e) const {
      return "RESTED    id=" + std::to_string(e.id) + ' ' + std::to_string(e.remaining) + " @ " +
             std::to_string(e.price);
    }
    std::string operator()(const lob::OrderCancelled& e) const {
      return "CANCELLED id=" + std::to_string(e.id) + ' ' + std::to_string(e.remaining) + " @ " +
             std::to_string(e.price) + ' ' + std::string(lob::to_string(e.reason));
    }
  };
  return "#" + std::to_string(lob::seq_of(event)) + ' ' + std::visit(Visitor{}, event);
}

void print_book(const lob::OrderBook& book) {
  const auto all = std::numeric_limits<std::size_t>::max();
  const auto asks = book.depth(Side::Sell, all);
  for (auto it = asks.rbegin(); it != asks.rend(); ++it) {
    std::printf("        ask %4lld x %-4lld (%zu)\n", static_cast<long long>(it->price),
                static_cast<long long>(it->total_quantity), it->order_count);
  }
  for (const auto& level : book.depth(Side::Buy, all)) {
    std::printf("        bid %4lld x %-4lld (%zu)\n", static_cast<long long>(level.price),
                static_cast<long long>(level.total_quantity), level.order_count);
  }
  if (book.empty()) {
    std::printf("        (empty)\n");
  }
}

lob::NewOrder order(lob::OrderId id, Side side, lob::Price price, lob::Quantity qty) {
  return lob::NewOrder{.id = id, .side = side, .price = price, .quantity = qty};
}

}  // namespace

int main() {
  lob::MatchingEngine engine{lob::EngineConfig{
      .min_price = 1, .max_price = 1'000, .max_order_quantity = 1'000, .max_live_orders = 6}};

  const std::vector<lob::Command> script{
      order(1, Side::Sell, 101, 5),
      order(2, Side::Sell, 101, 3),
      order(3, Side::Sell, 102, 4),
      order(4, Side::Buy, 99, 10),
      order(5, Side::Buy, 102, 10),   // sweeps 101 (FIFO: 1 then 2) and 102 (2 of 4)
      order(6, Side::Sell, 98, 12),   // sells into the bid at 99, remainder rests at 98
      lob::CancelOrder{.id = 3},
      lob::CancelOrder{.id = 3},      // already cancelled
      order(4, Side::Buy, 97, 1),     // id 4 is gone (fully filled): reuse is allowed
      order(6, Side::Buy, 50, 1),     // id 6 is resting: duplicate
      order(7, Side::Buy, 2'000, 1),  // outside the price band
      order(8, Side::Buy, 10, 1),
      order(9, Side::Buy, 11, 1),
      order(10, Side::Buy, 12, 1),
      order(11, Side::Buy, 13, 1),
      order(12, Side::Buy, 14, 1),    // 6 orders resting: capacity reached
  };

  std::vector<lob::Event> events;
  for (const lob::Command& command : script) {
    events.clear();
    engine.process(command, events);
    std::printf("%s\n", describe(command).c_str());
    for (const lob::Event& e : events) {
      std::printf("    %s\n", describe(e).c_str());
    }
    print_book(engine.book());
  }

  const auto violation = engine.find_invariant_violation();
  std::printf("last_seq=%llu invariants=%s\n", static_cast<unsigned long long>(engine.last_seq()),
              violation ? violation->c_str() : "OK");
  return violation ? 1 : 0;
}
