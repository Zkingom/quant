#pragma once

// GoogleTest glue for M3 tests: readable event printing (so a failing
// EXPECT_EQ on events shows fields instead of bytes) and command builders.
// Text formatting itself lives in lob::testkit (describe.hpp).

#include <gtest/gtest.h>

#include <ostream>
#include <string>
#include <vector>

#include "lob/engine/events.hpp"
#include "lob/engine/matching_engine.hpp"
#include "lob/testkit/describe.hpp"

namespace lob {

// Found by GoogleTest through ADL.
inline void PrintTo(const OrderAccepted& e, std::ostream* os) { *os << testkit::describe(Event{e}); }
inline void PrintTo(const OrderRejected& e, std::ostream* os) { *os << testkit::describe(Event{e}); }
inline void PrintTo(const TradeExecuted& e, std::ostream* os) { *os << testkit::describe(Event{e}); }
inline void PrintTo(const OrderRested& e, std::ostream* os) { *os << testkit::describe(Event{e}); }
inline void PrintTo(const OrderCancelled& e, std::ostream* os) { *os << testkit::describe(Event{e}); }
inline void PrintTo(const Event& e, std::ostream* os) { *os << testkit::describe(e); }

namespace test {

using testkit::describe;
using testkit::dump;

inline NewOrder buy(OrderId id, Price price, Quantity quantity) {
  return NewOrder{.id = id, .side = Side::Buy, .price = price, .quantity = quantity};
}

inline NewOrder sell(OrderId id, Price price, Quantity quantity) {
  return NewOrder{.id = id, .side = Side::Sell, .price = price, .quantity = quantity};
}

}  // namespace test
}  // namespace lob
