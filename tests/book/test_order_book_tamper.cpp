// Tamper tests: prove that find_invariant_violation() detects each class of
// corruption. The book's public API cannot produce these states, so the test
// reaches into private members through OrderBookTestAccess, which exists only
// in this file (DECISIONS D-015).
//
// Each test first asserts the book is consistent, applies exactly one
// corruption, then asserts the reported violation names that corruption.

#include <gtest/gtest.h>

#include <limits>
#include <optional>
#include <string>
#include <utility>

#include "book_test_util.hpp"
#include "lob/book/order_book.hpp"

namespace lob {

struct OrderBookTestAccess {
  static auto& bids(OrderBook& b) { return b.bids_; }
  static auto& asks(OrderBook& b) { return b.asks_; }
  static auto& index(OrderBook& b) { return b.index_; }
  static EngineConfig& config(OrderBook& b) { return b.config_; }
};

namespace {

using test::buy;
using test::sell;
using Access = OrderBookTestAccess;

constexpr EngineConfig kConfig{
    .min_price = 1, .max_price = 1'000, .max_order_quantity = 100, .max_live_orders = 10};

// Book: bids 100:[1x10, 2x20]  99:[3x5] ; asks 105:[4x7, 5x8]
OrderBook make_book() {
  OrderBook book{kConfig};
  book.add(buy(1, 100, 10), 1);
  book.add(buy(2, 100, 20), 2);
  book.add(buy(3, 99, 5), 3);
  book.add(sell(4, 105, 7), 4);
  book.add(sell(5, 105, 8), 5);
  return book;
}

::testing::AssertionResult reports(const OrderBook& book, const std::string& needle) {
  const std::optional<std::string> violation = book.find_invariant_violation();
  if (!violation) {
    return ::testing::AssertionFailure() << "no violation reported; expected '" << needle << "'";
  }
  if (violation->find(needle) == std::string::npos) {
    return ::testing::AssertionFailure() << "reported '" << *violation << "', expected '" << needle
                                         << "'";
  }
  return ::testing::AssertionSuccess() << *violation;
}

class TamperTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_EQ(book_.find_invariant_violation(), std::nullopt); }
  OrderBook book_ = make_book();
};

// Invariant 1 ------------------------------------------------------------

TEST_F(TamperTest, DetectsEmptyLevel) {
  Access::bids(book_).try_emplace(50);  // level with no orders
  EXPECT_TRUE(reports(book_, "BUY level 50: empty level"));
}

TEST_F(TamperTest, DetectsLevelPriceOutsideBand) {
  // Re-key the 105 ask level to 2000 without touching its nodes.
  auto node = Access::asks(book_).extract(105);
  node.key() = 2'000;
  Access::asks(book_).insert(std::move(node));
  EXPECT_TRUE(reports(book_, "SELL level 2000: price outside the configured band"));
}

// Invariant 2 ------------------------------------------------------------

TEST_F(TamperTest, DetectsLevelTotalMismatch) {
  Access::bids(book_).at(100).total_quantity += 1;
  EXPECT_TRUE(reports(book_, "BUY level 100: total_quantity 31 != sum of remaining 30"));
}

TEST_F(TamperTest, DetectsZeroRemaining) {
  auto& level = Access::asks(book_).at(105);
  level.orders.front().remaining = 0;
  level.total_quantity -= 7;  // keep the total consistent: only remaining is wrong
  EXPECT_TRUE(reports(book_, "order 4: remaining 0 out of range"));
}

TEST_F(TamperTest, DetectsRemainingAboveMaxOrderQuantity) {
  auto& level = Access::bids(book_).at(99);
  level.orders.front().remaining = kConfig.max_order_quantity + 1;
  level.total_quantity = kConfig.max_order_quantity + 1;
  EXPECT_TRUE(reports(book_, "order 3: remaining 101 out of range"));
}

// Invariant 3 ------------------------------------------------------------

TEST_F(TamperTest, DetectsFifoSequenceInversion) {
  auto& orders = Access::bids(book_).at(100).orders;
  std::swap(orders.front().seq, orders.back().seq);  // 1,2 -> 2,1
  EXPECT_TRUE(reports(book_, "order 2: seq 1 breaks FIFO order"));
}

TEST_F(TamperTest, DetectsSeqBeyondLastAdded) {
  Access::asks(book_).at(105).orders.back().seq = 999;
  EXPECT_TRUE(reports(book_, "order 5: seq 999 exceeds last added seq"));
}

// Invariant 4 ------------------------------------------------------------

TEST_F(TamperTest, DetectsOrderMissingFromIndex) {
  Access::index(book_).erase(3);
  EXPECT_TRUE(reports(book_, "order 3: missing from index"));
}

TEST_F(TamperTest, DetectsIndexPointingAtWrongNode) {
  auto& index = Access::index(book_);
  std::swap(index.at(1).order, index.at(2).order);  // same level, swapped nodes
  EXPECT_TRUE(reports(book_, "order 1: index entry does not point at this order"));
}

TEST_F(TamperTest, DetectsIndexWithWrongPrice) {
  Access::index(book_).at(3).price = 98;
  EXPECT_TRUE(reports(book_, "order 3: index entry does not point at this order"));
}

TEST_F(TamperTest, DetectsIndexWithWrongSide) {
  Access::index(book_).at(4).side = Side::Buy;
  EXPECT_TRUE(reports(book_, "order 4: index entry does not point at this order"));
}

TEST_F(TamperTest, DetectsIndexWithWrongLevelPointer) {
  auto& index = Access::index(book_);
  index.at(3).level = index.at(1).level;  // 99's order claims to live in level 100
  EXPECT_TRUE(reports(book_, "order 3: index entry does not point at this order"));
}

TEST_F(TamperTest, DetectsPhantomIndexEntry) {
  auto& index = Access::index(book_);
  const auto alias = index.at(5);  // copy first: emplace may rehash
  index.emplace(77, alias);        // extra entry aliasing a real order
  EXPECT_TRUE(reports(book_, "index has 6 entries but the book holds 5 orders"));
}

// Invariant 5 ------------------------------------------------------------

TEST_F(TamperTest, DetectsOrderCountAboveCapacity) {
  Access::config(book_).max_live_orders = 4;  // book holds 5
  EXPECT_TRUE(reports(book_, "order count exceeds max_live_orders"));
}

// Invariant 6 ------------------------------------------------------------

TEST_F(TamperTest, DetectsCrossedBook) {
  // Move the 105 ask level to 100 (== best bid) and keep its index entries
  // consistent, so crossing is the only violation.
  auto node = Access::asks(book_).extract(105);
  node.key() = 100;
  Access::asks(book_).insert(std::move(node));
  Access::index(book_).at(4).price = 100;
  Access::index(book_).at(5).price = 100;
  EXPECT_TRUE(reports(book_, "book is crossed: best bid 100 >= best ask 100"));
}

// Checker edge -----------------------------------------------------------

TEST_F(TamperTest, DetectsQuantitySumOverflowWithoutUndefinedBehaviour) {
  // Remaining values this large are themselves invalid, so relax the config
  // to reach the overflow branch; under UBSan an unchecked sum would abort.
  constexpr Quantity kHuge = std::numeric_limits<Quantity>::max() / 2 + 1;
  Access::config(book_).max_order_quantity = std::numeric_limits<Quantity>::max();
  auto& orders = Access::bids(book_).at(100).orders;
  orders.front().remaining = kHuge;
  orders.back().remaining = kHuge;
  EXPECT_TRUE(reports(book_, "BUY level 100: quantity sum overflows"));
}

}  // namespace
}  // namespace lob
