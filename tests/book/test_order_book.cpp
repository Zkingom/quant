#include <gtest/gtest.h>

#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "book_test_util.hpp"
#include "lob/book/order_book.hpp"

namespace lob {
namespace {

using test::buy;
using test::dump;
using test::sell;

static_assert(!std::is_copy_constructible_v<OrderBook>);
static_assert(!std::is_copy_assignable_v<OrderBook>);
static_assert(std::is_nothrow_move_constructible_v<OrderBook>);
static_assert(std::is_nothrow_move_assignable_v<OrderBook>);

constexpr EngineConfig kConfig{
    .min_price = 1, .max_price = 1'000, .max_order_quantity = 1'000, .max_live_orders = 100};

class OrderBookTest : public ::testing::Test {
 protected:
  // Adds with an automatically increasing seq, mirroring the engine.
  void add(const NewOrder& order) { book_.add(order, ++seq_); }

  OrderBook book_{kConfig};
  SeqNum seq_{0};
};

TEST(OrderBookConstruction, RejectsInvalidConfig) {
  EXPECT_THROW(OrderBook(EngineConfig{.min_price = 0}), std::invalid_argument);
}

TEST_F(OrderBookTest, EmptyBook) {
  EXPECT_TRUE(book_.empty());
  EXPECT_EQ(book_.order_count(), 0U);
  EXPECT_EQ(book_.best_bid(), std::nullopt);
  EXPECT_EQ(book_.best_ask(), std::nullopt);
  EXPECT_EQ(book_.front(Side::Buy), std::nullopt);
  EXPECT_EQ(book_.front(Side::Sell), std::nullopt);
  EXPECT_EQ(book_.level_count(Side::Buy), 0U);
  EXPECT_EQ(book_.quantity_at(Side::Buy, 100), 0);
  EXPECT_TRUE(book_.depth(Side::Sell, 10).empty());
  EXPECT_EQ(book_.cancel(1), std::nullopt);
  EXPECT_BOOK_CONSISTENT(book_);
}

TEST_F(OrderBookTest, BestPricesFollowPricePriority) {
  add(buy(1, 98, 5));
  add(buy(2, 100, 5));
  add(buy(3, 99, 5));
  add(sell(4, 103, 5));
  add(sell(5, 101, 5));
  add(sell(6, 102, 5));
  EXPECT_EQ(book_.best_bid(), 100);
  EXPECT_EQ(book_.best_ask(), 101);
  EXPECT_EQ(book_.front(Side::Buy)->id, 2U);
  EXPECT_EQ(book_.front(Side::Sell)->id, 5U);

  const std::vector<LevelSummary> bids{{100, 5, 1}, {99, 5, 1}, {98, 5, 1}};
  const std::vector<LevelSummary> asks{{101, 5, 1}, {102, 5, 1}, {103, 5, 1}};
  EXPECT_EQ(book_.depth(Side::Buy, 10), bids);
  EXPECT_EQ(book_.depth(Side::Sell, 10), asks);
  EXPECT_EQ(book_.depth(Side::Sell, 2).size(), 2U);
  EXPECT_TRUE(book_.depth(Side::Sell, 0).empty());
  EXPECT_BOOK_CONSISTENT(book_);
}

TEST_F(OrderBookTest, SamePriceQueuesInArrivalOrder) {
  add(sell(10, 101, 3));
  add(sell(11, 101, 4));
  add(sell(12, 101, 5));
  EXPECT_EQ(book_.level_count(Side::Sell), 1U);
  EXPECT_EQ(book_.quantity_at(Side::Sell, 101), 12);
  EXPECT_EQ(dump(book_), "SELL: 101/10x3#1 101/11x4#2 101/12x5#3\nBUY:\n");

  // Fully filling the head exposes the next order in arrival order.
  EXPECT_EQ(book_.fill_front(Side::Sell, 3), (Fill{10, 101, 3, 0}));
  EXPECT_EQ(book_.front(Side::Sell)->id, 11U);
  EXPECT_EQ(book_.fill_front(Side::Sell, 4), (Fill{11, 101, 4, 0}));
  EXPECT_EQ(book_.front(Side::Sell)->id, 12U);
  EXPECT_BOOK_CONSISTENT(book_);
}

TEST_F(OrderBookTest, PartialFillKeepsQueuePosition) {
  add(buy(1, 100, 10));
  add(buy(2, 100, 10));
  EXPECT_EQ(book_.fill_front(Side::Buy, 4), (Fill{1, 100, 4, 6}));
  EXPECT_TRUE(book_.contains(1));
  EXPECT_EQ(book_.front(Side::Buy), (OrderView{1, Side::Buy, 100, 6, 1}));
  EXPECT_EQ(book_.quantity_at(Side::Buy, 100), 16);
  EXPECT_BOOK_CONSISTENT(book_);
}

TEST_F(OrderBookTest, EmptiedLevelIsRemovedAndNextLevelBecomesBest) {
  add(sell(1, 101, 5));
  add(sell(2, 102, 7));
  EXPECT_EQ(book_.fill_front(Side::Sell, 5), (Fill{1, 101, 5, 0}));
  EXPECT_FALSE(book_.contains(1));
  EXPECT_EQ(book_.level_count(Side::Sell), 1U);
  EXPECT_EQ(book_.quantity_at(Side::Sell, 101), 0);
  EXPECT_EQ(book_.best_ask(), 102);
  EXPECT_EQ(book_.fill_front(Side::Sell, 7), (Fill{2, 102, 7, 0}));
  EXPECT_TRUE(book_.empty());
  EXPECT_EQ(book_.best_ask(), std::nullopt);
  EXPECT_BOOK_CONSISTENT(book_);
}

TEST_F(OrderBookTest, CancelFromHeadMiddleAndTailPreservesFifo) {
  for (OrderId id = 1; id <= 5; ++id) {
    add(buy(id, 100, static_cast<Quantity>(id)));
  }
  EXPECT_EQ(book_.cancel(3), (CancelledOrder{3, Side::Buy, 100, 3}));  // middle
  EXPECT_EQ(book_.cancel(1), (CancelledOrder{1, Side::Buy, 100, 1}));  // head
  EXPECT_EQ(book_.cancel(5), (CancelledOrder{5, Side::Buy, 100, 5}));  // tail
  EXPECT_EQ(dump(book_), "SELL:\nBUY: 100/2x2#2 100/4x4#4\n");
  EXPECT_EQ(book_.quantity_at(Side::Buy, 100), 6);
  EXPECT_BOOK_CONSISTENT(book_);
}

TEST_F(OrderBookTest, CancelLastOrderRemovesLevel) {
  add(sell(1, 101, 5));
  add(sell(2, 105, 5));
  ASSERT_TRUE(book_.cancel(1).has_value());
  EXPECT_EQ(book_.level_count(Side::Sell), 1U);
  EXPECT_EQ(book_.best_ask(), 105);
  EXPECT_BOOK_CONSISTENT(book_);
}

TEST_F(OrderBookTest, CancelReturnsRemainingAfterPartialFill) {
  add(sell(1, 101, 10));
  (void)book_.fill_front(Side::Sell, 7);
  EXPECT_EQ(book_.cancel(1), (CancelledOrder{1, Side::Sell, 101, 3}));
  EXPECT_TRUE(book_.empty());
  EXPECT_BOOK_CONSISTENT(book_);
}

TEST_F(OrderBookTest, CancelUnknownOrAlreadyRemovedIdReturnsNullopt) {
  add(buy(1, 100, 5));
  add(sell(2, 101, 5));
  EXPECT_EQ(book_.cancel(99), std::nullopt);
  ASSERT_TRUE(book_.cancel(1).has_value());
  EXPECT_EQ(book_.cancel(1), std::nullopt);  // double cancel
  (void)book_.fill_front(Side::Sell, 5);
  EXPECT_EQ(book_.cancel(2), std::nullopt);  // already fully filled
  EXPECT_TRUE(book_.empty());
  EXPECT_BOOK_CONSISTENT(book_);
}

TEST_F(OrderBookTest, IdMayBeReusedOnceNoLongerResting) {
  add(buy(1, 100, 5));
  ASSERT_TRUE(book_.cancel(1).has_value());
  add(buy(1, 90, 2));
  EXPECT_EQ(book_.find(1), (OrderView{1, Side::Buy, 90, 2, 2}));
  EXPECT_BOOK_CONSISTENT(book_);
}

TEST_F(OrderBookTest, FindReportsLocation) {
  add(buy(7, 100, 5));
  add(sell(8, 120, 9));
  EXPECT_EQ(book_.find(7), (OrderView{7, Side::Buy, 100, 5, 1}));
  EXPECT_EQ(book_.find(8), (OrderView{8, Side::Sell, 120, 9, 2}));
  EXPECT_EQ(book_.find(9), std::nullopt);
}

TEST_F(OrderBookTest, PriceBandEdgesAreUsable) {
  add(buy(1, kConfig.min_price, kConfig.max_order_quantity));
  add(sell(2, kConfig.max_price, kConfig.max_order_quantity));
  EXPECT_EQ(book_.best_bid(), kConfig.min_price);
  EXPECT_EQ(book_.best_ask(), kConfig.max_price);
  EXPECT_BOOK_CONSISTENT(book_);
}

// ------------------------------------------------- contract violations

class OrderBookContractTest : public OrderBookTest {
 protected:
  void SetUp() override {
    add(buy(1, 100, 10));
    add(buy(2, 99, 10));
    add(sell(3, 102, 10));
    before_ = dump(book_);
  }

  void expect_unchanged() {
    EXPECT_EQ(dump(book_), before_);
    EXPECT_BOOK_CONSISTENT(book_);
  }

  std::string before_;
};

TEST_F(OrderBookContractTest, AddRejectsInvalidOrder) {
  EXPECT_THROW(book_.add(buy(0, 100, 1), 10), std::logic_error);
  EXPECT_THROW(book_.add(buy(9, 0, 1), 10), std::logic_error);
  EXPECT_THROW(book_.add(buy(9, 100, 0), 10), std::logic_error);
  EXPECT_THROW(book_.add(buy(9, 100, kConfig.max_order_quantity + 1), 10), std::logic_error);
  NewOrder bad_side = buy(9, 100, 1);
  bad_side.side = static_cast<Side>(5);
  EXPECT_THROW(book_.add(bad_side, 10), std::logic_error);
  expect_unchanged();
}

TEST_F(OrderBookContractTest, AddRejectsDuplicateRestingId) {
  EXPECT_THROW(book_.add(buy(1, 95, 1), 10), std::logic_error);
  EXPECT_THROW(book_.add(sell(3, 110, 1), 10), std::logic_error);
  expect_unchanged();
}

TEST_F(OrderBookContractTest, AddRejectsCrossingOrLockingPrice) {
  EXPECT_THROW(book_.add(buy(9, 102, 1), 10), std::logic_error);   // locks best ask
  EXPECT_THROW(book_.add(buy(9, 150, 1), 10), std::logic_error);   // through best ask
  EXPECT_THROW(book_.add(sell(9, 100, 1), 10), std::logic_error);  // locks best bid
  EXPECT_THROW(book_.add(sell(9, 50, 1), 10), std::logic_error);   // through best bid
  expect_unchanged();
  EXPECT_NO_THROW(book_.add(buy(9, 101, 1), 10));   // inside the spread
  EXPECT_NO_THROW(book_.add(sell(10, 101 + 1, 1), 11));
  EXPECT_BOOK_CONSISTENT(book_);
}

TEST_F(OrderBookContractTest, AddRejectsNonIncreasingSeq) {
  EXPECT_THROW(book_.add(buy(9, 90, 1), seq_), std::logic_error);      // equal
  EXPECT_THROW(book_.add(buy(9, 90, 1), seq_ - 1), std::logic_error);  // smaller
  EXPECT_THROW(book_.add(buy(9, 90, 1), 0), std::logic_error);
  expect_unchanged();
  EXPECT_NO_THROW(book_.add(buy(9, 90, 1), seq_ + 100));  // gaps are fine
}

TEST_F(OrderBookContractTest, FillFrontRejectsBadQuantityAndEmptySide) {
  EXPECT_THROW((void)book_.fill_front(Side::Sell, 0), std::logic_error);
  EXPECT_THROW((void)book_.fill_front(Side::Sell, -1), std::logic_error);
  EXPECT_THROW((void)book_.fill_front(Side::Sell, 11), std::logic_error);
  EXPECT_THROW((void)book_.fill_front(static_cast<Side>(3), 1), std::logic_error);
  expect_unchanged();
  (void)book_.fill_front(Side::Sell, 10);
  EXPECT_THROW((void)book_.fill_front(Side::Sell, 1), std::logic_error);  // now empty
}

TEST(OrderBookCapacity, AddRejectsWhenFullAndRecoversAfterRemoval) {
  OrderBook book{EngineConfig{
      .min_price = 1, .max_price = 100, .max_order_quantity = 10, .max_live_orders = 2}};
  book.add(buy(1, 10, 1), 1);
  book.add(buy(2, 11, 1), 2);
  EXPECT_THROW(book.add(buy(3, 12, 1), 3), std::logic_error);
  EXPECT_EQ(book.order_count(), 2U);
  ASSERT_TRUE(book.cancel(1).has_value());
  EXPECT_NO_THROW(book.add(buy(3, 12, 1), 3));
  EXPECT_BOOK_CONSISTENT(book);
}

// ------------------------------------------------------------- lifecycle

TEST_F(OrderBookTest, MovedBookKeepsWorkingIndex) {
  add(buy(1, 100, 5));
  add(buy(2, 100, 6));
  add(sell(3, 101, 7));
  const std::string before = dump(book_);

  OrderBook moved{std::move(book_)};
  EXPECT_EQ(dump(moved), before);
  // The invariant check compares every Locator's level pointer and list
  // iterator against the address of the node it finds while walking the
  // destination's own maps, so passing it proves the index was not left
  // pointing into the source.
  EXPECT_BOOK_CONSISTENT(moved);
  EXPECT_EQ(moved.cancel(2), (CancelledOrder{2, Side::Buy, 100, 6}));
  EXPECT_EQ(moved.fill_front(Side::Sell, 7), (Fill{3, 101, 7, 0}));
  EXPECT_BOOK_CONSISTENT(moved);
}

TEST_F(OrderBookTest, MovedFromBookIsEmptyConsistentAndReusable) {
  add(buy(1, 100, 5));
  add(sell(2, 101, 5));
  OrderBook moved{std::move(book_)};

  // NOLINTBEGIN(bugprone-use-after-move): the specified source state is the
  // subject of this test.
  EXPECT_TRUE(book_.empty());
  EXPECT_EQ(book_.level_count(Side::Buy), 0U);
  EXPECT_EQ(book_.level_count(Side::Sell), 0U);
  EXPECT_EQ(book_.best_bid(), std::nullopt);
  EXPECT_EQ(book_.cancel(1), std::nullopt);  // must not reach the moved nodes
  EXPECT_EQ(book_.config(), kConfig);
  EXPECT_BOOK_CONSISTENT(book_);

  // The source keeps its last seq, so seq stays monotonic if it is reused.
  EXPECT_THROW(book_.add(buy(1, 100, 5), seq_), std::logic_error);
  book_.add(buy(1, 100, 5), ++seq_);  // same id as an order now owned by `moved`
  EXPECT_BOOK_CONSISTENT(book_);
  // NOLINTEND(bugprone-use-after-move)

  // The two books are fully independent.
  EXPECT_EQ(moved.find(1), (OrderView{1, Side::Buy, 100, 5, 1}));
  EXPECT_TRUE(moved.cancel(1).has_value());
  EXPECT_TRUE(book_.contains(1));
  EXPECT_BOOK_CONSISTENT(moved);
  EXPECT_BOOK_CONSISTENT(book_);
}

TEST_F(OrderBookTest, MoveAssignmentReplacesContentsAndClearsSource) {
  add(buy(1, 100, 5));
  OrderBook target{EngineConfig{
      .min_price = 1, .max_price = 600, .max_order_quantity = 10, .max_live_orders = 3}};
  target.add(sell(50, 500, 1), 1);
  target.add(sell(51, 501, 1), 2);

  target = std::move(book_);
  EXPECT_EQ(dump(target), "SELL:\nBUY: 100/1x5#1\n");
  EXPECT_EQ(target.config(), kConfig);  // config travels with the contents
  EXPECT_FALSE(target.contains(50));    // previous contents destroyed
  EXPECT_EQ(target.cancel(50), std::nullopt);
  EXPECT_BOOK_CONSISTENT(target);
  EXPECT_TRUE(book_.empty());  // NOLINT(bugprone-use-after-move)
  EXPECT_BOOK_CONSISTENT(book_);

  EXPECT_EQ(target.cancel(1), (CancelledOrder{1, Side::Buy, 100, 5}));
  EXPECT_TRUE(target.empty());
}

TEST_F(OrderBookTest, SelfMoveAssignmentIsANoOp) {
  add(buy(1, 100, 5));
  const std::string before = dump(book_);
  OrderBook& alias = book_;
  book_ = std::move(alias);
  EXPECT_EQ(dump(book_), before);
  EXPECT_BOOK_CONSISTENT(book_);
}

}  // namespace
}  // namespace lob
