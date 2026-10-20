// Self-tests for EventAuditor: each test feeds a stream that violates exactly
// one audited property and asserts the auditor names it. Without these, a
// silently permissive auditor would make every engine test look stronger
// than it is.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "engine_test_util.hpp"
#include "lob/testkit/event_auditor.hpp"
#include "lob/engine/matching_engine.hpp"

namespace lob {
namespace {

using test::buy;
using test::sell;

constexpr EngineConfig kConfig{
    .min_price = 1, .max_price = 1'000, .max_order_quantity = 1'000, .max_live_orders = 100};

class AuditorTest : public ::testing::Test {
 protected:
  // Setup commands go through a real engine so the auditor's state is built
  // from a genuine stream: sells 1@101 x5 (seq 1,2) and 2@100 x5 (seq 3,4).
  void SetUp() override {
    for (const Command& c : {Command{sell(1, 101, 5)}, Command{sell(2, 100, 5)}}) {
      std::vector<Event> events;
      engine_.process(c, events);
      ASSERT_EQ(auditor_.audit(c, events), std::nullopt);
    }
  }

  ::testing::AssertionResult expect_violation(const Command& command,
                                              const std::vector<Event>& events,
                                              const std::string& needle) {
    const std::optional<std::string> result = auditor_.audit(command, events);
    if (!result) {
      return ::testing::AssertionFailure() << "auditor accepted a bad stream; expected '" << needle
                                           << "'";
    }
    const std::string& message = *result;
    if (message.find(needle) == std::string::npos) {
      return ::testing::AssertionFailure() << "auditor said '" << message << "', expected '"
                                           << needle << "'";
    }
    return ::testing::AssertionSuccess() << message;
  }

  static Event accepted(SeqNum seq, const NewOrder& o) {
    return OrderAccepted{.seq = seq, .id = o.id, .side = o.side, .price = o.price, .quantity = o.quantity};
  }
  static Event trade(SeqNum seq, OrderId taker, OrderId maker, Price price, Quantity qty,
                     Quantity taker_rem, Quantity maker_rem) {
    return TradeExecuted{.seq = seq,
                         .taker_id = taker,
                         .maker_id = maker,
                         .taker_side = Side::Buy,
                         .price = price,
                         .quantity = qty,
                         .taker_remaining = taker_rem,
                         .maker_remaining = maker_rem};
  }

  MatchingEngine engine_{kConfig};
  testkit::EventAuditor auditor_{kConfig};
};

TEST_F(AuditorTest, AcceptsTheCorrectStream) {
  const NewOrder o = buy(3, 100, 2);
  const std::vector<Event> events{accepted(5, o), trade(6, 3, 2, 100, 2, 0, 3)};
  EXPECT_EQ(auditor_.audit(o, events), std::nullopt);
}

TEST_F(AuditorTest, DetectsSeqGap) {
  const NewOrder o = buy(3, 50, 1);
  EXPECT_TRUE(expect_violation(o, {accepted(6, o)}, "seq 6, expected 5"));
}

TEST_F(AuditorTest, DetectsPriceTimePriorityViolation) {
  const NewOrder o = buy(3, 101, 1);  // best ask is order 2 @100
  EXPECT_TRUE(expect_violation(o, {accepted(5, o), trade(6, 3, 1, 101, 1, 0, 4)},
                               "price-time priority violated"));
}

TEST_F(AuditorTest, DetectsTradeNotAtMakerPrice) {
  const NewOrder o = buy(3, 101, 1);
  EXPECT_TRUE(expect_violation(o, {accepted(5, o), trade(6, 3, 2, 101, 1, 0, 4)},
                               "trade price 101 != maker price 100"));
}

TEST_F(AuditorTest, DetectsQuantityThatIsNotTheMinimum) {
  const NewOrder o = buy(3, 100, 3);
  EXPECT_TRUE(expect_violation(o, {accepted(5, o), trade(6, 3, 2, 100, 2, 1, 3)},
                               "trade quantity 2, expected 3"));
}

TEST_F(AuditorTest, DetectsWrongRemainingInTrade) {
  const NewOrder o = buy(3, 100, 3);
  EXPECT_TRUE(expect_violation(o, {accepted(5, o), trade(6, 3, 2, 100, 3, 0, 5)},
                               "trade remaining quantities wrong"));
}

TEST_F(AuditorTest, DetectsRestingThroughCrossingLiquidity) {
  const NewOrder o = buy(3, 100, 4);
  const Event rested = OrderRested{.seq = 6, .id = 3, .side = Side::Buy, .price = 100, .remaining = 4};
  EXPECT_TRUE(expect_violation(o, {accepted(5, o), rested}, "still resting"));
}

TEST_F(AuditorTest, DetectsMissingTerminalEventForRemainder) {
  const NewOrder o = buy(3, 100, 7);  // fills 5, 2 must rest or be cancelled
  EXPECT_TRUE(expect_violation(o, {accepted(5, o), trade(6, 3, 2, 100, 5, 2, 0)},
                               "no RESTED/CANCELLED event"));
}

TEST_F(AuditorTest, DetectsWrongRestedQuantity) {
  const NewOrder o = buy(3, 99, 4);
  const Event rested = OrderRested{.seq = 6, .id = 3, .side = Side::Buy, .price = 99, .remaining = 3};
  EXPECT_TRUE(expect_violation(o, {accepted(5, o), rested}, "RESTED does not match"));
}

TEST_F(AuditorTest, DetectsEventAfterCompleteFill) {
  const NewOrder o = buy(3, 100, 5);
  const Event extra = OrderRested{.seq = 7, .id = 3, .side = Side::Buy, .price = 100, .remaining = 1};
  EXPECT_TRUE(expect_violation(o, {accepted(5, o), trade(6, 3, 2, 100, 5, 0, 0), extra},
                               "unexpected extra event"));
}

TEST_F(AuditorTest, DetectsAcceptedDuplicate) {
  const NewOrder o = sell(1, 200, 1);  // id 1 is resting
  EXPECT_TRUE(expect_violation(o, {accepted(5, o), OrderRested{.seq = 6, .id = 1, .side = Side::Sell,
                                                               .price = 200, .remaining = 1}},
                               "accepted an order that must be rejected"));
}

TEST_F(AuditorTest, DetectsWrongRejectionReason) {
  const NewOrder o = buy(1, 99, 1);  // duplicate of resting id 1
  const Event wrong = OrderRejected{
      .seq = 5, .command = CommandType::NewOrder, .id = 1, .reason = RejectReason::BookFull};
  EXPECT_TRUE(expect_violation(o, {wrong}, "rejection reason BOOK_FULL, expected DUPLICATE_ORDER_ID"));
}

TEST_F(AuditorTest, DetectsCancelOfOrderThatIsNotResting) {
  const Event cancelled = OrderCancelled{.seq = 5,
                                         .id = 9,
                                         .side = Side::Buy,
                                         .price = 100,
                                         .remaining = 1,
                                         .reason = CancelReason::Requested};
  EXPECT_TRUE(expect_violation(CancelOrder{.id = 9}, {cancelled}, "not resting"));
}

TEST_F(AuditorTest, DetectsCancelWithWrongRemaining) {
  const Event cancelled = OrderCancelled{.seq = 5,
                                         .id = 2,
                                         .side = Side::Sell,
                                         .price = 100,
                                         .remaining = 4,
                                         .reason = CancelReason::Requested};
  EXPECT_TRUE(expect_violation(CancelOrder{.id = 2}, {cancelled}, "CANCELLED does not match"));
}

TEST_F(AuditorTest, MatchesDetectsBookDivergence) {
  EXPECT_EQ(auditor_.matches(engine_.book()), std::nullopt);
  std::vector<Event> ignored;
  engine_.process(buy(3, 50, 1), ignored);  // the auditor never sees this
  EXPECT_NE(auditor_.matches(engine_.book()), std::nullopt);
}

}  // namespace
}  // namespace lob
