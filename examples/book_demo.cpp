// Demonstrates M2: resting orders, FIFO queues, filling the front of the book,
// cancelling, and the invariant check. The book has no matching logic; the
// fill_front() calls below play the role the matching engine (M3) will play.

#include <cstdio>
#include <exception>
#include <limits>
#include <string_view>

#include "lob/book/order_book.hpp"

namespace {

void print_ladder(const lob::OrderBook& book, std::string_view title) {
  std::printf("--- %.*s ---\n", static_cast<int>(title.size()), title.data());
  const auto all = std::numeric_limits<std::size_t>::max();
  const auto asks = book.depth(lob::Side::Sell, all);
  for (auto it = asks.rbegin(); it != asks.rend(); ++it) {  // worst ask on top
    std::printf("          %6lld | %5lld  (%zu orders)\n", static_cast<long long>(it->price),
                static_cast<long long>(it->total_quantity), it->order_count);
  }
  std::printf("  ------------------------\n");
  for (const auto& level : book.depth(lob::Side::Buy, all)) {
    std::printf("  %5lld | %6lld          (%zu orders)\n",
                static_cast<long long>(level.total_quantity), static_cast<long long>(level.price),
                level.order_count);
  }
  const auto violation = book.find_invariant_violation();
  std::printf("  orders=%zu invariants=%s\n", book.order_count(),
              violation ? violation->c_str() : "OK");
}

lob::NewOrder order(lob::OrderId id, lob::Side side, lob::Price price, lob::Quantity qty) {
  return lob::NewOrder{.id = id, .side = side, .price = price, .quantity = qty};
}

}  // namespace

int main() {
  using lob::Side;
  lob::OrderBook book{lob::EngineConfig{}};
  lob::SeqNum seq = 0;

  book.add(order(1, Side::Buy, 100, 10), ++seq);
  book.add(order(2, Side::Buy, 100, 5), ++seq);
  book.add(order(3, Side::Buy, 99, 20), ++seq);
  book.add(order(4, Side::Sell, 102, 8), ++seq);
  book.add(order(5, Side::Sell, 103, 12), ++seq);
  book.add(order(6, Side::Sell, 102, 4), ++seq);
  print_ladder(book, "after 6 adds");

  // An incoming sell for 12 would trade with the bid queue at 100: order 1
  // first (time priority), then order 2.
  for (const lob::Quantity q : {10, 2}) {
    const lob::Fill f = book.fill_front(Side::Buy, q);
    std::printf("fill maker=%llu price=%lld qty=%lld maker_remaining=%lld\n",
                static_cast<unsigned long long>(f.maker_id), static_cast<long long>(f.price),
                static_cast<long long>(f.quantity), static_cast<long long>(f.maker_remaining));
  }
  print_ladder(book, "after filling 12 against the bids");

  if (const auto c = book.cancel(4)) {
    std::printf("cancel id=%llu side=%s price=%lld remaining=%lld\n",
                static_cast<unsigned long long>(c->id), lob::to_string(c->side).data(),
                static_cast<long long>(c->price), static_cast<long long>(c->remaining));
  }
  std::printf("cancel id=4 again -> %s\n", book.cancel(4) ? "removed" : "not resting");
  print_ladder(book, "after cancelling order 4");

  try {
    book.add(order(7, Side::Buy, 102, 1), ++seq);  // would cross the best ask
  } catch (const std::exception& e) {
    std::printf("contract violation caught: %s\n", e.what());
  }
  print_ladder(book, "unchanged after the rejected add");
  return 0;
}
