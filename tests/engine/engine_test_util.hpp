#pragma once

// Test helpers for M3: readable event printing (so failing EXPECT_EQ on
// events shows fields instead of bytes), command builders, and a canonical
// dump of the engine's book.

#include <gtest/gtest.h>

#include <ostream>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include "lob/engine/events.hpp"
#include "lob/engine/matching_engine.hpp"

namespace lob {

// PrintTo overloads are found by GoogleTest through ADL.
inline void PrintTo(const OrderAccepted& e, std::ostream* os) {
  *os << '#' << e.seq << " ACCEPTED id=" << e.id << ' ' << to_string(e.side) << ' ' << e.price
      << 'x' << e.quantity;
}
inline void PrintTo(const OrderRejected& e, std::ostream* os) {
  *os << '#' << e.seq << " REJECTED " << to_string(e.command) << " id=" << e.id << ' '
      << to_string(e.reason);
}
inline void PrintTo(const TradeExecuted& e, std::ostream* os) {
  *os << '#' << e.seq << " TRADE taker=" << e.taker_id << '(' << to_string(e.taker_side)
      << ") maker=" << e.maker_id << ' ' << e.price << 'x' << e.quantity
      << " taker_rem=" << e.taker_remaining << " maker_rem=" << e.maker_remaining;
}
inline void PrintTo(const OrderRested& e, std::ostream* os) {
  *os << '#' << e.seq << " RESTED id=" << e.id << ' ' << to_string(e.side) << ' ' << e.price
      << 'x' << e.remaining;
}
inline void PrintTo(const OrderCancelled& e, std::ostream* os) {
  *os << '#' << e.seq << " CANCELLED id=" << e.id << ' ' << to_string(e.side) << ' ' << e.price
      << 'x' << e.remaining << ' ' << to_string(e.reason);
}
inline void PrintTo(const Event& e, std::ostream* os) {
  std::visit([os](const auto& alt) { PrintTo(alt, os); }, e);
}

namespace test {

inline std::string describe(const Event& e) {
  std::ostringstream s;
  PrintTo(e, &s);
  return s.str();
}

inline std::string describe(const std::vector<Event>& events) {
  std::string out;
  for (const Event& e : events) {
    out += describe(e);
    out += '\n';
  }
  return out;
}

inline NewOrder buy(OrderId id, Price price, Quantity quantity) {
  return NewOrder{.id = id, .side = Side::Buy, .price = price, .quantity = quantity};
}

inline NewOrder sell(OrderId id, Price price, Quantity quantity) {
  return NewOrder{.id = id, .side = Side::Sell, .price = price, .quantity = quantity};
}

/// Canonical text of the book, including each order's priority seq.
inline std::string dump(const OrderBook& book) {
  std::string out;
  for (const Side side : {Side::Sell, Side::Buy}) {
    out += to_string(side);
    out += ':';
    book.for_each_order(side, [&](Price price, const RestingOrder& o) {
      out += ' ' + std::to_string(price) + '/' + std::to_string(o.id) + 'x' +
             std::to_string(o.remaining) + '#' + std::to_string(o.seq);
    });
    out += '\n';
  }
  return out;
}

}  // namespace test
}  // namespace lob
