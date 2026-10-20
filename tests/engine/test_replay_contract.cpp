// Executable contract for M5 (persistence, replay, recovery): what a caller
// of MatchingEngine::process may rely on, and what it must guard against,
// especially when an allocation fails after a command has started to
// execute (partial fills, then the remainder cannot rest). DESIGN.md §5 lists
// the contract; each test below pins one clause. Two tests DEMONSTRATE
// hazards (C2, C3): they pass because the hazard is real, so M5 must handle
// it.

#include <gtest/gtest.h>

#include <algorithm>
#include <iterator>
#include <map>
#include <new>
#include <optional>
#include <string>
#include <vector>

#include "engine_test_util.hpp"
#include "lob/engine/matching_engine.hpp"
#include "lob/testkit/alloc_fault.hpp"
#include "lob/testkit/reference_engine.hpp"
#include "lob/testkit/workload.hpp"

namespace lob {
namespace {

using test::buy;
using test::dump;
using test::sell;
using testkit::alloc::FaultWindow;

constexpr EngineConfig kConfig{
    .min_price = 1, .max_price = 100, .max_order_quantity = 100, .max_live_orders = 50};

// Outcome of one process() call as the caller can observe it.
struct Attempt {
  bool threw = false;
  std::vector<Event> events;
  SeqNum seq_before = 0;
  SeqNum seq_after = 0;
};

Attempt attempt(MatchingEngine& engine, const Command& command, std::optional<long> fail_at,
                bool reserve) {
  Attempt a;
  if (reserve) {
    a.events.reserve(256);
  }
  a.seq_before = engine.last_seq();
  try {
    if (fail_at) {
      FaultWindow window{*fail_at};
      engine.process(command, a.events);
    } else {
      engine.process(command, a.events);
    }
  } catch (const std::bad_alloc&) {
    a.threw = true;
  }
  a.seq_after = engine.last_seq();
  return a;
}

void feed(MatchingEngine& engine, const std::vector<Command>& commands) {
  std::vector<Event> ignored;
  for (const Command& c : commands) {
    engine.process(c, ignored);
  }
}

// ------------------------------------------------------------------ C1

// C1: if process() throws and last_seq() did not move, the command had no
// effect at all; retrying it is safe and gives the result a failure-free
// run would have given.
TEST(ReplayContract, C1NotAppliedCommandCanBeRetried) {
  const std::vector<Command> prefix{sell(1, 50, 5), buy(2, 40, 5)};
  const Command command = buy(3, 50, 8);  // trades 5, rests 3

  MatchingEngine failed{kConfig};
  feed(failed, prefix);
  const std::string book_before = dump(failed.book());
  const Attempt first = attempt(failed, command, 0, /*reserve=*/false);  // event capacity fails
  ASSERT_TRUE(first.threw);
  EXPECT_EQ(first.seq_after, first.seq_before);
  EXPECT_TRUE(first.events.empty());
  EXPECT_EQ(dump(failed.book()), book_before);

  const Attempt retry = attempt(failed, command, std::nullopt, false);
  MatchingEngine clean{kConfig};
  feed(clean, prefix);
  const Attempt reference = attempt(clean, command, std::nullopt, false);
  EXPECT_EQ(retry.events, reference.events);
  EXPECT_EQ(dump(failed.book()), dump(clean.book()));
}

// ------------------------------------------------------------------ C2

// C2 (hazard): if process() throws and last_seq() moved, the command WAS
// applied: its trades stand and its events (ending in CANCELLED
// {INSUFFICIENT_RESOURCES}) were appended. Re-submitting it executes it a
// second time. M5 must therefore persist those events and must not retry.
TEST(ReplayContract, C2RetryAfterAppliedFailureExecutesTwice) {
  MatchingEngine engine{kConfig};
  feed(engine, {sell(1, 50, 5)});  // seqs 1, 2
  const Command command = buy(10, 50, 8);

  // Reserved buffer: the first allocation inside process() is OrderBook::add
  // for the remainder 3, after the trade with order 1 has executed.
  const Attempt first = attempt(engine, command, 0, /*reserve=*/true);
  ASSERT_TRUE(first.threw);
  EXPECT_NE(first.seq_after, first.seq_before);  // => applied
  const std::vector<Event> expected_first{
      OrderAccepted{.seq = 3, .id = 10, .side = Side::Buy, .price = 50, .quantity = 8},
      TradeExecuted{.seq = 4,
                    .taker_id = 10,
                    .maker_id = 1,
                    .taker_side = Side::Buy,
                    .price = 50,
                    .quantity = 5,
                    .taker_remaining = 3,
                    .maker_remaining = 0},
      OrderCancelled{.seq = 5,
                     .id = 10,
                     .side = Side::Buy,
                     .price = 50,
                     .remaining = 3,
                     .reason = CancelReason::InsufficientResources}};
  EXPECT_EQ(first.events, expected_first);
  EXPECT_TRUE(engine.book().empty());

  // A naive retry is accepted again (id 10 is not resting) and buys 8 more:
  // the client ends up with 5 filled + 8 resting instead of 5 + 3.
  const Attempt retry = attempt(engine, command, std::nullopt, true);
  EXPECT_FALSE(retry.threw);
  EXPECT_EQ(retry.events,
            (std::vector<Event>{
                OrderAccepted{.seq = 6, .id = 10, .side = Side::Buy, .price = 50, .quantity = 8},
                OrderRested{.seq = 7, .id = 10, .side = Side::Buy, .price = 50, .remaining = 8}}));
}

// ------------------------------------------------------------------ C3

// C3 (hazard): re-executing the COMMAND log does not reproduce a run that
// had a resource failure: the failed command's remainder rests in the replay
// but was cancelled in the original, and later commands can trade against
// it. Command-log replay is only valid for failure-free runs; recovery must
// use the event log (C4) or treat the failure outcome as part of the input.
TEST(ReplayContract, C3CommandReplayDivergesAfterAResourceFailure) {
  const std::vector<Command> log{sell(1, 50, 5), buy(10, 50, 8), sell(20, 50, 2), sell(21, 49, 1)};
  const std::size_t failing = 1;  // buy 10: trades 5, then its remainder 3 fails to rest

  MatchingEngine original{kConfig};
  std::vector<std::vector<Event>> original_events;
  for (std::size_t i = 0; i < log.size(); ++i) {
    const Attempt a = attempt(original, log[i], i == failing ? std::optional<long>{0} : std::nullopt,
                              /*reserve=*/true);
    ASSERT_EQ(a.threw, i == failing);
    original_events.push_back(a.events);
  }

  MatchingEngine replay{kConfig};
  std::vector<std::vector<Event>> replay_events;
  for (const Command& c : log) {
    replay_events.push_back(attempt(replay, c, std::nullopt, true).events);
  }

  EXPECT_EQ(original_events[0], replay_events[0]);           // identical before the failure
  EXPECT_NE(original_events[failing], replay_events[failing]);  // RESTED vs CANCELLED
  EXPECT_TRUE(std::holds_alternative<OrderRested>(replay_events[failing].back()));
  // Later commands diverge too: sell 20 trades with the replayed remainder of
  // buy 10, while in the original it rests (nothing to trade with).
  EXPECT_NE(original_events[2], replay_events[2]);
  EXPECT_NE(dump(original.book()), dump(replay.book()));
}

// ------------------------------------------------------------------ C4 / C5

// Rebuilds the resting book from the EVENT LOG ALONE (no commands, no
// engine): Accepted gives side/price/priority, Rested makes it rest, Trade
// sets the maker's remaining, Cancelled(Requested) removes it.
struct Rebuilt {
  struct Order {
    Side side;
    Price price;
    Quantity remaining;
    SeqNum seq;
  };
  std::map<OrderId, Order> resting;
  std::map<OrderId, OrderAccepted> pending;  // accepted, not yet resting
  SeqNum last_seq = 0;

  void apply(const Event& e) {
    last_seq = seq_of(e);
    if (const auto* a = std::get_if<OrderAccepted>(&e)) {
      pending[a->id] = *a;
    } else if (const auto* t = std::get_if<TradeExecuted>(&e)) {
      if (t->maker_remaining == 0) {
        resting.erase(t->maker_id);
      } else {
        resting.at(t->maker_id).remaining = t->maker_remaining;
      }
    } else if (const auto* r = std::get_if<OrderRested>(&e)) {
      const OrderAccepted& acceptance = pending.at(r->id);
      resting[r->id] = Order{acceptance.side, acceptance.price, r->remaining, acceptance.seq};
      pending.erase(r->id);
    } else if (const auto* c = std::get_if<OrderCancelled>(&e)) {
      resting.erase(c->id);  // INSUFFICIENT_RESOURCES: was never resting; no-op
      pending.erase(c->id);
    }
  }

  std::string dump() const {
    std::vector<std::pair<OrderId, Order>> v(resting.begin(), resting.end());
    std::string out;
    for (const Side side : {Side::Sell, Side::Buy}) {
      std::vector<std::pair<OrderId, Order>> s;
      std::copy_if(v.begin(), v.end(), std::back_inserter(s),
                   [side](const auto& x) { return x.second.side == side; });
      std::sort(s.begin(), s.end(), [side](const auto& a, const auto& b) {
        if (a.second.price != b.second.price) {
          return side == Side::Buy ? a.second.price > b.second.price : a.second.price < b.second.price;
        }
        return a.second.seq < b.second.seq;
      });
      out += to_string(side);
      out += ':';
      for (const auto& [id, o] : s) {
        out += ' ' + std::to_string(o.price) + '/' + std::to_string(id) + 'x' +
               std::to_string(o.remaining) + '#' + std::to_string(o.seq);
      }
      out += '\n';
    }
    return out;
  }
};

// C4: the event log alone is sufficient to rebuild the exact book (incl.
// priority seq), also across many resource failures. C5: the events of one
// process() call are exactly the seqs (last_seq before, last_seq after],
// contiguous, whether or not it threw. Run over a long faulted workload.
TEST(ReplayContract, C4C5EventLogRebuildsBookAndSeqRangesAttributeEvents) {
  const testkit::WorkloadProfile& profile = *testkit::find_profile("mixed");
  for (std::uint64_t seed = 1; seed <= 3; ++seed) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    MatchingEngine engine{profile.config};
    testkit::ReferenceEngine model{profile.config};  // only to steer the generator
    testkit::WorkloadGenerator generator{profile, seed};
    testkit::Rng faults{seed + 1'000};
    std::vector<Event> log;  // the persisted event log
    Rebuilt rebuilt;
    long applied_failures = 0;

    for (long step = 0; step < 5'000; ++step) {
      const Command command = generator.next(step, model);
      const bool inject = faults.percent(5);
      const Attempt a =
          attempt(engine, command, inject ? std::optional<long>{faults.uniform(0, 5)} : std::nullopt,
                  /*reserve=*/faults.percent(50));

      // C5
      ASSERT_EQ(a.events.size(), a.seq_after - a.seq_before) << "step " << step;
      for (std::size_t i = 0; i < a.events.size(); ++i) {
        ASSERT_EQ(seq_of(a.events[i]), a.seq_before + 1 + i) << "step " << step;
      }
      // Keep the generator's model aligned with what actually happened.
      if (!a.events.empty()) {
        model.process(command);
        if (a.threw) {
          ++applied_failures;
          model.drop_resting(std::get<OrderCancelled>(a.events.back()).id);
        }
      }
      log.insert(log.end(), a.events.begin(), a.events.end());
      for (const Event& e : a.events) {
        rebuilt.apply(e);
      }
      if (step % 250 == 0 || a.threw) {
        ASSERT_EQ(rebuilt.dump(), dump(engine.book())) << "step " << step;  // C4
        ASSERT_EQ(rebuilt.last_seq, engine.last_seq());
      }
    }
    ASSERT_EQ(rebuilt.dump(), dump(engine.book()));
    EXPECT_GT(applied_failures, 0);  // the clause was exercised across failures

    // And from the persisted log, replayed into a fresh rebuild:
    Rebuilt from_disk;
    for (const Event& e : log) {
      from_disk.apply(e);
    }
    EXPECT_EQ(from_disk.dump(), dump(engine.book()));
  }
}

// ------------------------------------------------------------------ C6

// C6: without resource failures, re-executing the same command log in a new
// engine reproduces the event log exactly, independent of how the caller
// manages its event buffer (append vs. clear per command, reserved or not).
TEST(ReplayContract, C6FailureFreeCommandReplayIsExact) {
  const testkit::WorkloadProfile& profile = *testkit::find_profile("high_density");
  testkit::ReferenceEngine model{profile.config};
  testkit::WorkloadGenerator generator{profile, 7};
  std::vector<Command> commands;
  for (long step = 0; step < 5'000; ++step) {
    commands.push_back(generator.next(step, model));
    model.process(commands.back());
  }

  MatchingEngine a{profile.config};
  std::vector<Event> appended;
  for (const Command& c : commands) {
    a.process(c, appended);
  }
  MatchingEngine b{profile.config};
  std::vector<Event> collected;
  std::vector<Event> per_command;
  for (const Command& c : commands) {
    per_command.clear();
    b.process(c, per_command);
    collected.insert(collected.end(), per_command.begin(), per_command.end());
  }
  EXPECT_EQ(appended, collected);
  EXPECT_EQ(dump(a.book()), dump(b.book()));
  EXPECT_EQ(a.last_seq(), b.last_seq());
}

}  // namespace
}  // namespace lob
