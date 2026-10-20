#pragma once

#include <gtest/gtest.h>

#include <string>

#include "lob/book/order_book.hpp"

namespace lob::test {

/// Canonical text of the whole book (both sides, priority order). Used to
/// assert that a failed operation left the book unchanged.
inline std::string dump(const OrderBook& book) {
  std::string out;
  for (const Side side : {Side::Sell, Side::Buy}) {
    out += to_string(side);
    out += ':';
    book.for_each_order(side, [&](Price price, const RestingOrder& order) {
      out += ' ' + std::to_string(price) + '/' + std::to_string(order.id) + 'x' +
             std::to_string(order.remaining) + '#' + std::to_string(order.seq);
    });
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

}  // namespace lob::test

#define EXPECT_BOOK_CONSISTENT(book) EXPECT_EQ((book).find_invariant_violation(), std::nullopt)
