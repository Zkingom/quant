#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "engine_test_util.hpp"
#include "lob/testkit/event_auditor.hpp"
#include "lob/engine/matching_engine.hpp"

namespace lob {

struct MatchingEngineTestAccess {
  static void set_last_seq(MatchingEngine& engine, SeqNum seq) { engine.last_seq_ = seq; }
};

namespace {

using test::buy;
using test::dump;
using test::sell;

static_assert(!std::is_copy_constructible_v<MatchingEngine>);
static_assert(std::is_nothrow_move_constructible_v<MatchingEngine>);
static_assert(std::is_nothrow_move_assignable_v<MatchingEngine>);

constexpr EngineConfig kConfig{
    .min_price = 1, .max_price = 1'000, .max_order_quantity = 1'000, .max_live_orders = 100};

// ------------------------------------------------------- event builders

Event acc(SeqNum seq, OrderId id, Side side, Price price, Quantity qty) {
  return OrderAccepted{.seq = seq, .id = id, .side = side, .price = price, .quantity = qty};
}
Event rested(SeqNum seq, OrderId id, Side side, Price price, Quantity rem) {
  return OrderRested{.seq = seq, .id = id, .side = side, .price = price, .remaining = rem};
}
Event trade(SeqNum seq, OrderId taker, OrderId maker, Side taker_side, Price price, Quantity qty,
            Quantity taker_rem, Quantity maker_rem) {
  return TradeExecuted{.seq = seq,
                       .taker_id = taker,
                       .maker_id = maker,
                       .taker_side = taker_side,
                       .price = price,
                       .quantity = qty,
                       .taker_remaining = taker_rem,
                       .maker_remaining = maker_rem};
}
Event rej(SeqNum seq, CommandType command, OrderId id, RejectReason reason) {
  return OrderRejected{.seq = seq, .command = command, .id = id, .reason = reason};
}
Event cxl(SeqNum seq, OrderId id, Side side, Price price, Quantity rem) {
  return OrderCancelled{.seq = seq,
                        .id = id,
                        .side = side,
                        .price = price,
                        .remaining = rem,
                        .reason = CancelReason::Requested};
}

constexpr auto kBuy = Side::Buy;
constexpr auto kSell = Side::Sell;
constexpr auto kNew = CommandType::NewOrder;
constexpr auto kCancel = CommandType::CancelOrder;

/// Engine + auditor + full event log. run() processes one command, returns
/// only that command's events, and checks the audit properties (A1–A6) and
/// the engine invariants after every command.
struct Harness {
  explicit Harness(const EngineConfig& config) : engine(config), auditor(config) {}

  std::vector<Event> run(const Command& command) {
    const std::size_t before = log.size();
    engine.process(command, log);
    std::vector<Event> produced(log.begin() + static_cast<std::ptrdiff_t>(before), log.end());
    EXPECT_EQ(auditor.audit(command, produced), std::nullopt) << test::describe(produced);
    EXPECT_EQ(auditor.matches(engine.book()), std::nullopt);
    EXPECT_EQ(engine.find_invariant_violation(), std::nullopt);
    return produced;
  }

  MatchingEngine engine;
  testkit::EventAuditor auditor;
  std::vector<Event> log;
};

class EngineTest : public ::testing::Test {
 protected:
  std::vector<Event> run(const Command& command) { return h_.run(command); }
  const OrderBook& book() const { return h_.engine.book(); }
  Harness h_{kConfig};
};

// ---------------------------------------------------------- resting

TEST_F(EngineTest, OrderWithoutLiquidityRests) {
  EXPECT_EQ(run(buy(1, 100, 10)), (std::vector{acc(1, 1, kBuy, 100, 10), rested(2, 1, kBuy, 100, 10)}));
  EXPECT_EQ(run(sell(2, 101, 7)), (std::vector{acc(3, 2, kSell, 101, 7), rested(4, 2, kSell, 101, 7)}));
  EXPECT_EQ(book().best_bid(), 100);
  EXPECT_EQ(book().best_ask(), 101);
  EXPECT_EQ(h_.engine.last_seq(), 4U);
}

TEST_F(EngineTest, NonCrossingLimitDoesNotTrade) {
  run(sell(1, 101, 5));
  EXPECT_EQ(run(buy(2, 100, 5)), (std::vector{acc(3, 2, kBuy, 100, 5), rested(4, 2, kBuy, 100, 5)}));
  run(buy(3, 50, 5));
  EXPECT_EQ(run(sell(4, 101, 5)), (std::vector{acc(7, 4, kSell, 101, 5), rested(8, 4, kSell, 101, 5)}));
  EXPECT_EQ(book().order_count(), 4U);
}

// ---------------------------------------------------- fills and price

TEST_F(EngineTest, FullFillExecutesAtMakerPrice) {
  run(sell(1, 100, 10));
  // Buyer is willing to pay 105; the trade happens at the maker's 100.
  EXPECT_EQ(run(buy(2, 105, 10)),
            (std::vector{acc(3, 2, kBuy, 105, 10), trade(4, 2, 1, kBuy, 100, 10, 0, 0)}));
  EXPECT_TRUE(book().empty());
}

TEST_F(EngineTest, SellTakerExecutesAtMakerBidPrice) {
  run(buy(1, 100, 10));
  EXPECT_EQ(run(sell(2, 90, 10)),
            (std::vector{acc(3, 2, kSell, 90, 10), trade(4, 2, 1, kSell, 100, 10, 0, 0)}));
  EXPECT_TRUE(book().empty());
}

TEST_F(EngineTest, EqualPriceTrades) {
  run(sell(1, 100, 3));
  EXPECT_EQ(run(buy(2, 100, 3)),
            (std::vector{acc(3, 2, kBuy, 100, 3), trade(4, 2, 1, kBuy, 100, 3, 0, 0)}));
}

TEST_F(EngineTest, TakerPartialFillRestsRemainder) {
  run(sell(1, 100, 4));
  EXPECT_EQ(run(buy(2, 100, 10)), (std::vector{acc(3, 2, kBuy, 100, 10),
                                               trade(4, 2, 1, kBuy, 100, 4, 6, 0),
                                               rested(5, 2, kBuy, 100, 6)}));
  EXPECT_EQ(book().best_bid(), 100);
  EXPECT_EQ(book().best_ask(), std::nullopt);
  EXPECT_EQ(book().find(2)->remaining, 6);
}

TEST_F(EngineTest, MakerPartialFillKeepsQueuePosition) {
  run(sell(1, 100, 10));
  run(sell(2, 100, 5));
  EXPECT_EQ(run(buy(3, 100, 4)),
            (std::vector{acc(5, 3, kBuy, 100, 4), trade(6, 3, 1, kBuy, 100, 4, 0, 6)}));
  // Order 1 is still first in the queue with 6 left.
  EXPECT_EQ(run(buy(4, 100, 8)), (std::vector{acc(7, 4, kBuy, 100, 8),
                                              trade(8, 4, 1, kBuy, 100, 6, 2, 0),
                                              trade(9, 4, 2, kBuy, 100, 2, 0, 3)}));
  EXPECT_EQ(book().find(2)->remaining, 3);
}

// ----------------------------------------------------------- priority

TEST_F(EngineTest, BetterPriceTradesFirst) {
  run(sell(1, 102, 1));
  run(sell(2, 101, 1));
  run(sell(3, 103, 1));
  EXPECT_EQ(run(buy(4, 103, 3)), (std::vector{acc(7, 4, kBuy, 103, 3),
                                              trade(8, 4, 2, kBuy, 101, 1, 2, 0),
                                              trade(9, 4, 1, kBuy, 102, 1, 1, 0),
                                              trade(10, 4, 3, kBuy, 103, 1, 0, 0)}));
}

TEST_F(EngineTest, SamePriceTradesInArrivalOrder) {
  run(buy(1, 100, 2));
  run(buy(2, 100, 2));
  run(buy(3, 100, 2));
  EXPECT_EQ(run(sell(4, 100, 5)), (std::vector{acc(7, 4, kSell, 100, 5),
                                               trade(8, 4, 1, kSell, 100, 2, 3, 0),
                                               trade(9, 4, 2, kSell, 100, 2, 1, 0),
                                               trade(10, 4, 3, kSell, 100, 1, 0, 1)}));
}

TEST_F(EngineTest, PriorityIsTheAcceptanceSeq) {
  run(sell(1, 100, 2));
  // Order 2 trades first (seq 4) and rests later (seq 5); its priority is 3.
  EXPECT_EQ(run(buy(2, 100, 5)), (std::vector{acc(3, 2, kBuy, 100, 5),
                                              trade(4, 2, 1, kBuy, 100, 2, 3, 0),
                                              rested(5, 2, kBuy, 100, 3)}));
  EXPECT_EQ(book().find(2)->seq, 3U);
}

// ---------------------------------------------------------- multi-level

TEST_F(EngineTest, SweepStopsAtLimitAndRestsRemainder) {
  run(sell(1, 100, 5));
  run(sell(2, 101, 5));
  run(sell(3, 101, 5));
  run(sell(4, 102, 5));
  EXPECT_EQ(run(buy(5, 101, 20)), (std::vector{acc(9, 5, kBuy, 101, 20),
                                               trade(10, 5, 1, kBuy, 100, 5, 15, 0),
                                               trade(11, 5, 2, kBuy, 101, 5, 10, 0),
                                               trade(12, 5, 3, kBuy, 101, 5, 5, 0),
                                               rested(13, 5, kBuy, 101, 5)}));
  EXPECT_EQ(dump(book()), "SELL: 102/4x5#7\nBUY: 101/5x5#9\n");
}

TEST_F(EngineTest, SellSweepAcrossBidLevels) {
  run(buy(1, 100, 3));
  run(buy(2, 99, 3));
  run(buy(3, 98, 3));
  EXPECT_EQ(run(sell(4, 99, 5)), (std::vector{acc(7, 4, kSell, 99, 5),
                                              trade(8, 4, 1, kSell, 100, 3, 2, 0),
                                              trade(9, 4, 2, kSell, 99, 2, 0, 1)}));
  EXPECT_EQ(dump(book()), "SELL:\nBUY: 99/2x1#3 98/3x3#5\n");
}

TEST_F(EngineTest, SweepEmptiesWholeSideThenRests) {
  for (OrderId id = 1; id <= 50; ++id) {
    run(sell(id, 100 + static_cast<Price>(id), 1));
  }
  const auto events = run(buy(99, 1'000, 60));
  ASSERT_EQ(events.size(), 52U);  // accepted + 50 trades + rested
  EXPECT_EQ(events.back(), rested(152, 99, kBuy, 1'000, 10));
  EXPECT_EQ(book().level_count(kSell), 0U);
  EXPECT_EQ(book().order_count(), 1U);
}

// ---------------------------------------------------------- rejections

TEST_F(EngineTest, StatelessRejectionsLeaveBookUnchanged) {
  run(sell(1, 100, 5));
  const std::string before = dump(book());
  NewOrder bad_side = buy(9, 100, 1);
  bad_side.side = static_cast<Side>(3);
  const std::vector<std::pair<NewOrder, RejectReason>> cases{
      {buy(0, 100, 1), RejectReason::InvalidOrderId},
      {bad_side, RejectReason::InvalidSide},
      {buy(9, 0, 1), RejectReason::PriceOutOfRange},
      {buy(9, 1'001, 1), RejectReason::PriceOutOfRange},
      {buy(9, 100, 0), RejectReason::InvalidQuantity},
      {buy(9, 100, -5), RejectReason::InvalidQuantity},
      {buy(9, 100, 1'001), RejectReason::QuantityTooLarge},
  };
  SeqNum seq = h_.engine.last_seq();
  for (const auto& [order, reason] : cases) {
    EXPECT_EQ(run(order), (std::vector{rej(++seq, kNew, order.id, reason)}));
  }
  EXPECT_EQ(dump(book()), before);
}

TEST_F(EngineTest, DuplicateLiveIdIsRejectedEvenIfItWouldTrade) {
  run(sell(1, 100, 5));
  EXPECT_EQ(run(buy(1, 100, 5)), (std::vector{rej(3, kNew, 1, RejectReason::DuplicateOrderId)}));
  EXPECT_EQ(book().find(1)->remaining, 5);  // no trade happened
}

TEST_F(EngineTest, StatelessCheckPrecedesDuplicateCheck) {
  run(sell(1, 100, 5));
  EXPECT_EQ(run(buy(1, 100, 0)), (std::vector{rej(3, kNew, 1, RejectReason::InvalidQuantity)}));
}

TEST_F(EngineTest, IdsOfFilledAndCancelledOrdersMayBeReused) {
  run(sell(1, 100, 5));
  run(buy(2, 100, 5));  // fills 1
  EXPECT_EQ(run(sell(1, 101, 5)), (std::vector{acc(5, 1, kSell, 101, 5), rested(6, 1, kSell, 101, 5)}));
  run(CancelOrder{.id = 1});
  EXPECT_EQ(run(buy(1, 90, 2)), (std::vector{acc(8, 1, kBuy, 90, 2), rested(9, 1, kBuy, 90, 2)}));
}

// -------------------------------------------------------------- cancel

TEST_F(EngineTest, CancelRestingOrder) {
  run(buy(1, 100, 10));
  EXPECT_EQ(run(CancelOrder{.id = 1}), (std::vector{cxl(3, 1, kBuy, 100, 10)}));
  EXPECT_TRUE(book().empty());
}

TEST_F(EngineTest, CancelAfterPartialFillReturnsRemaining) {
  run(sell(1, 100, 10));
  run(buy(2, 100, 7));
  EXPECT_EQ(run(CancelOrder{.id = 1}), (std::vector{cxl(5, 1, kSell, 100, 3)}));
}

TEST_F(EngineTest, CancelRejections) {
  run(sell(1, 100, 5));
  run(buy(2, 100, 5));  // order 1 fully filled
  run(buy(3, 90, 5));   // seqs 5, 6
  EXPECT_EQ(run(CancelOrder{.id = 0}), (std::vector{rej(7, kCancel, 0, RejectReason::InvalidOrderId)}));
  EXPECT_EQ(run(CancelOrder{.id = 1}), (std::vector{rej(8, kCancel, 1, RejectReason::UnknownOrderId)}));
  EXPECT_EQ(run(CancelOrder{.id = 77}), (std::vector{rej(9, kCancel, 77, RejectReason::UnknownOrderId)}));
  EXPECT_EQ(run(CancelOrder{.id = 3}), (std::vector{cxl(10, 3, kBuy, 90, 5)}));
  EXPECT_EQ(run(CancelOrder{.id = 3}), (std::vector{rej(11, kCancel, 3, RejectReason::UnknownOrderId)}));
}

// ------------------------------------------------------------ capacity

class EngineCapacityTest : public ::testing::Test {
 protected:
  // Capacity 3: two bids and one ask make the book full.
  void SetUp() override {
    h_.run(buy(1, 90, 5));
    h_.run(buy(2, 91, 5));
    h_.run(sell(3, 100, 5));
    ASSERT_EQ(h_.engine.book().order_count(), 3U);
  }
  Harness h_{EngineConfig{
      .min_price = 1, .max_price = 1'000, .max_order_quantity = 1'000, .max_live_orders = 3}};
};

TEST_F(EngineCapacityTest, OrderThatWouldRestIsRejectedWhenFull) {
  const std::string before = dump(h_.engine.book());
  EXPECT_EQ(h_.run(buy(4, 95, 1)), (std::vector{rej(7, kNew, 4, RejectReason::BookFull)}));
  EXPECT_EQ(h_.run(sell(5, 120, 1)), (std::vector{rej(8, kNew, 5, RejectReason::BookFull)}));
  EXPECT_EQ(dump(h_.engine.book()), before);
}

TEST_F(EngineCapacityTest, FullyMatchingOrderIsAcceptedWhenFull) {
  EXPECT_EQ(h_.run(buy(4, 100, 2)),
            (std::vector{acc(7, 4, kBuy, 100, 2), trade(8, 4, 3, kBuy, 100, 2, 0, 3)}));
}

TEST_F(EngineCapacityTest, FillsFreeSlotsBeforeTheRemainderRests) {
  // Filling order 3 completely frees one slot, so the remainder fits.
  EXPECT_EQ(h_.run(buy(4, 100, 8)), (std::vector{acc(7, 4, kBuy, 100, 8),
                                                 trade(8, 4, 3, kBuy, 100, 5, 3, 0),
                                                 rested(9, 4, kBuy, 100, 3)}));
  EXPECT_EQ(h_.engine.book().order_count(), 3U);
}

TEST_F(EngineCapacityTest, LastFreeSlotCanBeUsed) {
  h_.run(CancelOrder{.id = 1});  // 2 resting, capacity 3
  EXPECT_EQ(h_.run(buy(4, 95, 1)), (std::vector{acc(8, 4, kBuy, 95, 1), rested(9, 4, kBuy, 95, 1)}));
  EXPECT_EQ(h_.run(buy(5, 95, 1)), (std::vector{rej(10, kNew, 5, RejectReason::BookFull)}));
}

// ----------------------------------------------------- seq and events

TEST_F(EngineTest, EventsAreAppendedAndSeqIsContiguous) {
  std::vector<Event> events{rej(999, kNew, 0, RejectReason::InvalidOrderId)};  // caller's data
  MatchingEngine engine{kConfig};
  engine.process(buy(1, 100, 1), events);
  engine.process(CancelOrder{.id = 1}, events);
  engine.process(CancelOrder{.id = 1}, events);
  ASSERT_EQ(events.size(), 5U);
  EXPECT_EQ(events[0], rej(999, kNew, 0, RejectReason::InvalidOrderId));  // untouched
  for (std::size_t i = 1; i < events.size(); ++i) {
    EXPECT_EQ(seq_of(events[i]), i);
  }
}

TEST_F(EngineTest, EventCapacityGrowsGeometrically) {
  // A caller that never clears the vector must not trigger a reallocation
  // per command (exact reserve would make appending N events O(N^2)).
  MatchingEngine engine{kConfig};
  std::vector<Event> events;
  int reallocations = 0;
  std::size_t capacity = events.capacity();
  for (int i = 0; i < 4'000; ++i) {
    engine.process(CancelOrder{.id = 12345}, events);  // one event each
    if (events.capacity() != capacity) {
      ++reallocations;
      capacity = events.capacity();
    }
  }
  EXPECT_EQ(events.size(), 4'000U);
  EXPECT_LE(reallocations, 14);  // log2(4000) ~ 12
}

TEST_F(EngineTest, CommandVariantDispatch) {
  EXPECT_EQ(run(Command{sell(1, 100, 1)}),
            (std::vector{acc(1, 1, kSell, 100, 1), rested(2, 1, kSell, 100, 1)}));
  EXPECT_EQ(run(Command{CancelOrder{.id = 1}}), (std::vector{cxl(3, 1, kSell, 100, 1)}));
}

TEST(EngineSeqExhaustion, RefusesToWrapAndChangesNothing) {
  constexpr SeqNum kMax = std::numeric_limits<SeqNum>::max();
  MatchingEngine engine{kConfig};
  std::vector<Event> events;
  engine.process(sell(1, 100, 5), events);
  MatchingEngineTestAccess::set_last_seq(engine, kMax - 1);
  const std::string before = dump(engine.book());
  events.clear();

  // Needs 2 seqs (accepted + rested), only 1 is left.
  EXPECT_THROW(engine.process(buy(2, 90, 1), events), std::overflow_error);
  // Needs 2 seqs (accepted + trade).
  EXPECT_THROW(engine.process(buy(3, 100, 5), events), std::overflow_error);
  EXPECT_TRUE(events.empty());
  EXPECT_EQ(dump(engine.book()), before);
  EXPECT_EQ(engine.last_seq(), kMax - 1);

  // A rejection needs exactly one seq: the last one is usable.
  engine.process(CancelOrder{.id = 42}, events);
  EXPECT_EQ(events, (std::vector{rej(kMax, kCancel, 42, RejectReason::UnknownOrderId)}));
  EXPECT_THROW(engine.process(CancelOrder{.id = 1}, events), std::overflow_error);
  EXPECT_EQ(events.size(), 1U);
  EXPECT_EQ(dump(engine.book()), before);
}

// ------------------------------------------------------------ lifecycle

TEST(EngineLifecycle, ConstructionValidatesConfig) {
  EXPECT_THROW(MatchingEngine(EngineConfig{.max_order_quantity = 0}), std::invalid_argument);
}

TEST(EngineLifecycle, MoveTransfersBookAndSeq) {
  MatchingEngine engine{kConfig};
  std::vector<Event> events;
  engine.process(sell(1, 100, 5), events);
  engine.process(buy(2, 90, 5), events);

  MatchingEngine moved{std::move(engine)};
  EXPECT_EQ(moved.last_seq(), 4U);
  EXPECT_EQ(moved.book().order_count(), 2U);
  EXPECT_EQ(moved.find_invariant_violation(), std::nullopt);
  events.clear();
  moved.process(buy(3, 100, 5), events);  // trades with order 1 in the moved book
  EXPECT_EQ(events, (std::vector{acc(5, 3, kBuy, 100, 5), trade(6, 3, 1, kBuy, 100, 5, 0, 0)}));

  // Moved-from: empty book, seq kept, still usable and independent.
  EXPECT_TRUE(engine.book().empty());  // NOLINT(bugprone-use-after-move)
  EXPECT_EQ(engine.last_seq(), 4U);
  events.clear();
  engine.process(buy(1, 50, 1), events);  // id 1 is free in this engine
  EXPECT_EQ(events, (std::vector{acc(5, 1, kBuy, 50, 1), rested(6, 1, kBuy, 50, 1)}));
  EXPECT_EQ(engine.find_invariant_violation(), std::nullopt);
}

}  // namespace
}  // namespace lob
