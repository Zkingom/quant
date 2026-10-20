// Fixed-seed differential test: MatchingEngine vs. ReferenceEngine, with the
// EventAuditor checking every command's events on top.
//
// For every command of a seeded random stream:
//   - the complete event vector of the engine must equal the reference's;
//   - both books must be equal order by order (id, price, remaining, seq);
//   - the auditor must accept the events and match the engine's book
//     (priority, maker price, lifecycle, conservation);
//   - MatchingEngine::find_invariant_violation() must report nothing.
// Coverage counters assert that the generator reached every rule; the test
// fails if one of them is zero, so a generator regression cannot silently
// weaken it.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <set>
#include <vector>

#include "engine_test_util.hpp"
#include "event_auditor.hpp"
#include "lob/engine/matching_engine.hpp"
#include "reference_engine.hpp"
#include "support/rng.hpp"

namespace lob {
namespace {

using test::ReferenceEngine;
using test::Rng;

constexpr EngineConfig kConfig{
    .min_price = 1, .max_price = 60, .max_order_quantity = 100, .max_live_orders = 40};

constexpr int kSeeds = 20;
constexpr int kCommandsPerSeed = 5'000;

struct Coverage {
  long commands = 0;
  long accepted = 0;
  long trades = 0;
  long multi_maker_commands = 0;   // >= 2 trades in one command
  long multi_level_commands = 0;   // trades at >= 2 distinct prices in one command
  long taker_fully_filled = 0;
  long taker_partial_then_rested = 0;
  long rested_without_trade = 0;
  long maker_partially_filled = 0;
  long accepted_while_full = 0;    // book at capacity before an accepted order
  long reused_dead_id = 0;
  long cancels = 0;
  std::array<long, 8> rejects{};   // indexed by RejectReason
  std::size_t max_orders = 0;
};

std::vector<ReferenceEngine::Resting> engine_side(const OrderBook& book, Side side) {
  std::vector<ReferenceEngine::Resting> out;
  book.for_each_order(side, [&](Price price, const RestingOrder& o) {
    out.push_back(ReferenceEngine::Resting{o.id, side, price, o.remaining, o.seq});
  });
  return out;
}

class Driver {
 public:
  explicit Driver(std::uint64_t seed) : rng_(seed) {}

  ::testing::AssertionResult run(int commands, Coverage& cov) {
    std::vector<Event> events;
    for (int step = 0; step < commands; ++step) {
      // Alternate 500-command phases: "build" fills the book towards the
      // capacity limit (passive orders, few cancels) so that BOOK_FULL and
      // trading at capacity are reached; "churn" sweeps and cancels.
      // Once full, "build" behaves like "churn" so commands are not wasted
      // on a stream of BOOK_FULL rejections.
      building_ = (step / 500) % 2 == 0 &&
                  static_cast<std::int64_t>(ref_.order_count()) < kConfig.max_live_orders;
      const Command command = next_command();
      const bool was_full = static_cast<std::int64_t>(ref_.order_count()) >= kConfig.max_live_orders;
      const auto ref_before = ref_.orders();  // for the BOOK_FULL property below

      events.clear();
      engine_.process(command, events);
      const std::vector<Event> expected = ref_.process(command);

      auto failure = [&](const std::string& what) {
        return ::testing::AssertionFailure()
               << "step " << step << ": " << what << "\nengine:\n"
               << test::describe(events) << "reference:\n"
               << test::describe(expected);
      };
      if (events != expected) {
        return failure("event streams differ");
      }
      for (const Side side : {Side::Buy, Side::Sell}) {
        if (engine_side(engine_.book(), side) != ref_.side_in_priority(side)) {
          return failure(std::string(to_string(side)) + " books differ");
        }
      }
      if (auto ok = auditor_.audit(command, events); !ok) {
        return failure(std::string("audit: ") + ok.message());
      }
      if (auto ok = auditor_.matches(engine_.book()); !ok) {
        return failure(std::string("audit book: ") + ok.message());
      }
      if (const auto violation = engine_.find_invariant_violation()) {
        return failure("invariant: " + *violation);
      }
      if (auto ok = record(command, events, was_full, ref_before, cov); !ok) {
        return failure(ok.message());
      }
    }
    return ::testing::AssertionSuccess();
  }

 private:
  // --------------------------------------------------------- generator

  Price mid() const {
    const auto bids = ref_.side_in_priority(Side::Buy);
    const auto asks = ref_.side_in_priority(Side::Sell);
    if (!bids.empty() && !asks.empty()) {
      return (bids.front().price + asks.front().price) / 2;
    }
    if (!bids.empty()) {
      return bids.front().price + 1;
    }
    if (!asks.empty()) {
      return asks.front().price - 1;
    }
    return 30;
  }

  static Price clamp_price(Price p) { return std::clamp<Price>(p, kConfig.min_price, kConfig.max_price); }

  /// A price that does not cross the opposite best (if the spread allows).
  Price passive_price(Side side) const {
    const auto opposite_side = ref_.side_in_priority(opposite(side));
    const Price gap = 1 + static_cast<Price>(passive_offset_);
    if (opposite_side.empty()) {
      return clamp_price(side == Side::Buy ? mid() - gap : mid() + gap);
    }
    const Price best = opposite_side.front().price;
    return clamp_price(side == Side::Buy ? best - gap : best + gap);
  }

  Command next_command() {
    const int roll = static_cast<int>(rng_.uniform(0, 99));
    if (building_) {
      if (roll < 5) {
        return next_cancel();
      }
      if (roll < 92) {
        passive_offset_ = rng_.uniform(0, 8);
        const Side side = rng_.side();
        return NewOrder{.id = next_id_++,
                        .side = side,
                        .price = passive_price(side),
                        .quantity = rng_.uniform(1, 30)};
      }
      // remaining 8%: fall through to the churn mix (sweeps, invalid, ...)
    }
    if (roll < 25 && !building_) {
      return next_cancel();
    }
    NewOrder order{.id = 0, .side = rng_.side(), .price = 0, .quantity = 0};
    // Id: mostly fresh; sometimes an old one (dead -> reuse, live -> duplicate).
    order.id = (next_id_ > 1 && rng_.percent(10))
                   ? static_cast<OrderId>(rng_.uniform(1, static_cast<std::int64_t>(next_id_) - 1))
                   : next_id_++;
    const Price m = mid();
    if (roll < 40) {
      // Aggressive: deep through the opposite side, large size (sweeps).
      const Price depth = rng_.uniform(3, 20);
      order.price = clamp_price(order.side == Side::Buy ? m + depth : m - depth);
      order.quantity = rng_.uniform(30, kConfig.max_order_quantity);
    } else {
      order.price = clamp_price(m + rng_.uniform(-8, 8));
      order.quantity = rng_.percent(70) ? rng_.uniform(1, 20) : rng_.uniform(21, 100);
    }
    if (roll >= 94) {  // stateless-invalid field
      switch (rng_.uniform(0, 4)) {
        case 0:
          order.id = 0;
          break;
        case 1:
          order.side = static_cast<Side>(rng_.uniform(2, 255));
          break;
        case 2:
          order.price = rng_.percent(50) ? 0 : kConfig.max_price + 1;
          break;
        case 3:
          order.quantity = rng_.percent(50) ? 0 : -rng_.uniform(1, 5);
          break;
        default:
          order.quantity = kConfig.max_order_quantity + 1;
          break;
      }
    }
    return order;
  }

  Command next_cancel() {
    const auto& orders = ref_.orders();
    if (!orders.empty() && rng_.percent(70)) {
      const auto pick = rng_.uniform(0, static_cast<std::int64_t>(orders.size()) - 1);
      return CancelOrder{.id = orders[static_cast<std::size_t>(pick)].id};
    }
    return CancelOrder{.id = static_cast<OrderId>(rng_.uniform(0, static_cast<std::int64_t>(next_id_) + 3))};
  }

  // ---------------------------------------------------------- coverage

  ::testing::AssertionResult record(const Command& command, const std::vector<Event>& events,
                                    bool was_full,
                                    const std::vector<ReferenceEngine::Resting>& before,
                                    Coverage& cov) {
    ++cov.commands;
    cov.max_orders = std::max(cov.max_orders, ref_.order_count());
    if (const auto* r = std::get_if<OrderRejected>(&events.front())) {
      ++cov.rejects[static_cast<std::size_t>(r->reason)];
      if (r->reason == RejectReason::BookFull) {
        // Property (DESIGN §3.4): BOOK_FULL only ever refuses orders that
        // would not have traded at all.
        const auto& o = std::get<NewOrder>(command);
        for (const auto& m : before) {
          const bool tradable = m.side != o.side &&
                                (o.side == Side::Buy ? m.price <= o.price : m.price >= o.price);
          if (tradable) {
            return ::testing::AssertionFailure()
                   << "BOOK_FULL refused order " << o.id << " that could trade with " << m.id;
          }
        }
      }
      return ::testing::AssertionSuccess();
    }
    if (std::holds_alternative<OrderCancelled>(events.front())) {
      ++cov.cancels;
      return ::testing::AssertionSuccess();
    }
    const auto& accepted = std::get<OrderAccepted>(events.front());
    ++cov.accepted;
    cov.accepted_while_full += was_full ? 1 : 0;
    if (accepted.id < next_id_ - 1 && seen_.contains(accepted.id)) {
      ++cov.reused_dead_id;
    }
    seen_.insert(accepted.id);

    std::set<Price> prices;
    long trades = 0;
    for (const Event& e : events) {
      if (const auto* t = std::get_if<TradeExecuted>(&e)) {
        ++trades;
        prices.insert(t->price);
        cov.maker_partially_filled += t->maker_remaining > 0 ? 1 : 0;
      }
    }
    cov.trades += trades;
    cov.multi_maker_commands += trades >= 2 ? 1 : 0;
    cov.multi_level_commands += prices.size() >= 2 ? 1 : 0;
    const bool rested = std::holds_alternative<OrderRested>(events.back());
    cov.taker_fully_filled += !rested ? 1 : 0;
    cov.taker_partial_then_rested += (rested && trades > 0) ? 1 : 0;
    cov.rested_without_trade += (rested && trades == 0) ? 1 : 0;
    return ::testing::AssertionSuccess();
  }

  Rng rng_;
  MatchingEngine engine_{kConfig};
  ReferenceEngine ref_{kConfig};
  test::EventAuditor auditor_{kConfig};
  OrderId next_id_{1};
  std::set<OrderId> seen_;
  bool building_{true};
  std::int64_t passive_offset_{0};
};

TEST(EngineDifferential, MatchesReferenceEngineOnFixedSeeds) {
  Coverage cov;
  for (int seed = 1; seed <= kSeeds; ++seed) {
    Driver driver{static_cast<std::uint64_t>(seed)};
    ASSERT_TRUE(driver.run(kCommandsPerSeed, cov)) << "seed " << seed;
  }

  auto reason = [&](RejectReason r) { return cov.rejects[static_cast<std::size_t>(r)]; };
  std::printf(
      "[coverage] seeds=%d commands=%ld accepted=%ld trades=%ld cancels=%ld\n"
      "[coverage] multi_maker=%ld multi_level=%ld taker_full=%ld taker_partial_rested=%ld "
      "rested_no_trade=%ld maker_partial=%ld\n"
      "[coverage] accepted_while_full=%ld reused_dead_id=%ld max_orders=%zu\n"
      "[coverage] rejects: invalid_id=%ld invalid_side=%ld price=%ld qty_invalid=%ld "
      "qty_large=%ld duplicate=%ld unknown=%ld book_full=%ld\n",
      kSeeds, cov.commands, cov.accepted, cov.trades, cov.cancels, cov.multi_maker_commands,
      cov.multi_level_commands, cov.taker_fully_filled, cov.taker_partial_then_rested,
      cov.rested_without_trade, cov.maker_partially_filled, cov.accepted_while_full,
      cov.reused_dead_id, cov.max_orders, reason(RejectReason::InvalidOrderId),
      reason(RejectReason::InvalidSide), reason(RejectReason::PriceOutOfRange),
      reason(RejectReason::InvalidQuantity), reason(RejectReason::QuantityTooLarge),
      reason(RejectReason::DuplicateOrderId), reason(RejectReason::UnknownOrderId),
      reason(RejectReason::BookFull));

  EXPECT_GT(cov.multi_maker_commands, 0);
  EXPECT_GT(cov.multi_level_commands, 0);
  EXPECT_GT(cov.taker_fully_filled, 0);
  EXPECT_GT(cov.taker_partial_then_rested, 0);
  EXPECT_GT(cov.rested_without_trade, 0);
  EXPECT_GT(cov.maker_partially_filled, 0);
  EXPECT_GT(cov.accepted_while_full, 0);
  EXPECT_GT(cov.reused_dead_id, 0);
  EXPECT_GT(cov.cancels, 0);
  for (std::size_t r = 0; r < cov.rejects.size(); ++r) {
    EXPECT_GT(cov.rejects[r], 0) << "reject reason never produced: "
                                 << to_string(static_cast<RejectReason>(r));
  }
  EXPECT_EQ(cov.max_orders, static_cast<std::size_t>(kConfig.max_live_orders));
}

}  // namespace
}  // namespace lob
