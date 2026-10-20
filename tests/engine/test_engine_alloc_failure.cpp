// Allocation-failure injection for MatchingEngine (DESIGN.md §3.5).
//
// For a command C applied after a prefix P of commands, sweep k = 0, 1, ...:
// rebuild engine + reference + auditor by replaying P (injection disarmed),
// then run C with "fail the k-th allocation" armed. Each outcome must be one
// of exactly two classes:
//
//   PREPARE   bad_alloc escaped and NOTHING changed: events vector identical
//             (contents), book identical, last_seq identical.  [strong]
//   RESIDUAL  bad_alloc escaped after the commit started: the appended events
//             equal the reference engine's events for C with its final
//             OrderRested replaced by OrderCancelled{InsufficientResources}
//             (same seq/id/side/price/remaining); the engine book equals the
//             reference book without that order; invariants hold; the auditor
//             accepts the stream; the engine keeps working.  [basic + audit]
//
// Any other outcome fails the test. The sweep stops at the first k for which
// C completes; asserting "injection points == allocations of the clean run"
// proves every allocation point of C was injected.

#include <gtest/gtest.h>

#include <cstdio>
#include <functional>
#include <new>
#include <string>
#include <vector>

#include "engine_test_util.hpp"
#include "event_auditor.hpp"
#include "lob/engine/matching_engine.hpp"
#include "reference_engine.hpp"
#include "support/alloc_fault.hpp"
#include "support/rng.hpp"

namespace lob {
namespace {

using test::alloc::FaultWindow;
using test::buy;
using test::dump;
using test::sell;

constexpr EngineConfig kConfig{
    .min_price = 1, .max_price = 100, .max_order_quantity = 100, .max_live_orders = 50};

enum class Buffer { Empty, PreReserved };  // state of the caller's events vector

struct SweepResult {
  long points = 0;            // injected k values
  long prepare = 0;           // PREPARE outcomes
  long residual = 0;          // RESIDUAL outcomes
  long clean_allocations = -1;
};

struct World {
  MatchingEngine engine{kConfig};
  test::ReferenceEngine ref{kConfig};
  test::EventAuditor auditor{kConfig};
};

void replay(World& w, const std::vector<Command>& prefix) {
  std::vector<Event> scratch;
  for (const Command& c : prefix) {
    scratch.clear();
    w.engine.process(c, scratch);
    const auto expected = w.ref.process(c);
    ASSERT_EQ(scratch, expected);
    ASSERT_TRUE(w.auditor.audit(c, scratch));
  }
}

std::vector<test::ReferenceEngine::Resting> engine_orders(const OrderBook& book, Side side) {
  std::vector<test::ReferenceEngine::Resting> out;
  book.for_each_order(side, [&](Price price, const RestingOrder& o) {
    out.push_back({o.id, side, price, o.remaining, o.seq});
  });
  return out;
}

SweepResult sweep(const std::vector<Command>& prefix, const Command& command, Buffer buffer,
                  const std::string& label) {
  SweepResult result;
  for (long k = 0; k < 200; ++k) {
    World w;
    replay(w, prefix);
    std::vector<Event> events{OrderRejected{.seq = 0}};  // caller-owned content
    if (buffer == Buffer::PreReserved) {
      events.reserve(512);
    } else {
      events.shrink_to_fit();  // capacity == size: the engine must grow it
    }
    const std::vector<Event> events_before = events;
    const std::string book_before = dump(w.engine.book());
    const SeqNum seq_before = w.engine.last_seq();

    bool threw = false;
    long allocations = 0;
    {
      FaultWindow window{k};
      try {
        w.engine.process(command, events);
      } catch (const std::bad_alloc&) {
        threw = true;
      }
      allocations = window.allocations();
    }
    if (!threw) {
      result.clean_allocations = allocations;
      // The clean run must also agree with the reference.
      const std::vector<Event> appended(events.begin() + 1, events.end());
      EXPECT_EQ(appended, w.ref.process(command)) << label;
      break;
    }
    ++result.points;
    SCOPED_TRACE(label + " k=" + std::to_string(k));

    if (events == events_before) {
      // PREPARE: nothing may have changed.
      ++result.prepare;
      EXPECT_EQ(dump(w.engine.book()), book_before);
      EXPECT_EQ(w.engine.last_seq(), seq_before);
    } else {
      // RESIDUAL: compare with the reference's outcome for the same command.
      ++result.residual;
      std::vector<Event> expected = w.ref.process(command);
      const auto* rested = expected.empty() ? nullptr : std::get_if<OrderRested>(&expected.back());
      if (rested == nullptr) {
        ADD_FAILURE() << "events changed although the reference does not rest the order:\n"
                      << test::describe(events);
        return result;
      }
      const OrderRested r = *rested;
      expected.back() = OrderCancelled{.seq = r.seq,
                                       .id = r.id,
                                       .side = r.side,
                                       .price = r.price,
                                       .remaining = r.remaining,
                                       .reason = CancelReason::InsufficientResources};
      const std::vector<Event> appended(events.begin() + 1, events.end());
      EXPECT_EQ(appended, expected);
      EXPECT_EQ(events.front(), events_before.front());  // caller data untouched
      EXPECT_EQ(w.engine.last_seq(), r.seq);

      // Engine book == reference book without the order that failed to rest.
      w.ref.process(CancelOrder{.id = r.id});
      for (const Side side : {Side::Buy, Side::Sell}) {
        EXPECT_EQ(engine_orders(w.engine.book(), side), w.ref.side_in_priority(side));
      }
      EXPECT_EQ(w.engine.find_invariant_violation(), std::nullopt);
      EXPECT_TRUE(w.auditor.audit(command, appended));
      EXPECT_TRUE(w.auditor.matches(w.engine.book()));

      // Still fully usable afterwards.
      std::vector<Event> more;
      EXPECT_NO_THROW(w.engine.process(buy(9'999, 1, 1), more));
      EXPECT_EQ(w.engine.find_invariant_violation(), std::nullopt);
    }
  }
  EXPECT_GE(result.clean_allocations, 0) << label << ": sweep did not terminate";
  EXPECT_EQ(result.points, result.clean_allocations) << label << ": not every point injected";
  return result;
}

// Book used by the targeted scenarios: asks 50x5 (id 1), 51x5 (id 2), 52x5
// (id 3); bids 40x5 (id 4), 39x5 (id 5).
const std::vector<Command> kPrefix{sell(1, 50, 5), sell(2, 51, 5), sell(3, 52, 5), buy(4, 40, 5),
                                   buy(5, 39, 5)};

TEST(EngineAllocFailure, RestWithoutTradeEmptyBuffer) {
  const auto r = sweep(kPrefix, buy(10, 45, 5), Buffer::Empty, "rest, empty buffer");
  EXPECT_GT(r.prepare, 0);   // event-capacity growth fails before any change
  EXPECT_GT(r.residual, 0);  // OrderBook::add fails after Accepted
}

TEST(EngineAllocFailure, RestWithoutTradePreReservedBuffer) {
  const auto r = sweep(kPrefix, buy(10, 45, 5), Buffer::PreReserved, "rest, reserved buffer");
  EXPECT_EQ(r.prepare, 0);   // nothing to allocate in prepare
  EXPECT_GT(r.residual, 0);  // only OrderBook::add allocates
}

TEST(EngineAllocFailure, MultiLevelSweepThenRest) {
  const auto r = sweep(kPrefix, buy(10, 51, 14), Buffer::Empty, "sweep + rest");
  EXPECT_GT(r.prepare, 0);
  EXPECT_GT(r.residual, 0);  // trades stand, remainder cancelled
}

TEST(EngineAllocFailure, FullFillAcrossLevelsAllocatesOnlyInPrepare) {
  const auto empty = sweep(kPrefix, buy(10, 52, 12), Buffer::Empty, "full fill, empty buffer");
  EXPECT_GT(empty.prepare, 0);
  EXPECT_EQ(empty.residual, 0);
  const auto reserved = sweep(kPrefix, buy(10, 52, 12), Buffer::PreReserved, "full fill, reserved");
  EXPECT_EQ(reserved.clean_allocations, 0);  // commit phase allocates nothing
}

TEST(EngineAllocFailure, CancelAndRejectionsDoNotAllocateWithReservedBuffer) {
  const std::vector<std::pair<Command, std::string>> cases{
      {CancelOrder{.id = 2}, "cancel"},
      {CancelOrder{.id = 77}, "cancel unknown"},
      {buy(1, 45, 5), "duplicate id"},
      {buy(10, 45, 0), "invalid quantity"},
  };
  for (const auto& [command, label] : cases) {
    const auto reserved = sweep(kPrefix, command, Buffer::PreReserved, label + ", reserved");
    EXPECT_EQ(reserved.clean_allocations, 0) << label;
    const auto empty = sweep(kPrefix, command, Buffer::Empty, label + ", empty");
    EXPECT_EQ(empty.residual, 0) << label;  // strong in every case
  }
}

TEST(EngineAllocFailure, BookFullRejectionHasStrongGuarantee) {
  std::vector<Command> prefix;
  const EngineConfig small{
      .min_price = 1, .max_price = 100, .max_order_quantity = 100, .max_live_orders = 50};
  for (OrderId id = 1; id <= static_cast<OrderId>(small.max_live_orders); ++id) {
    prefix.push_back(buy(id, 10 + static_cast<Price>(id % 20), 1));
  }
  const auto r = sweep(prefix, buy(1'000, 5, 1), Buffer::Empty, "book full");
  EXPECT_EQ(r.residual, 0);
}

// Broad sweep: every command of a seeded random stream, swept at every
// allocation point, with the stream so far as prefix.
TEST(EngineAllocFailure, EveryCommandOfARandomStream) {
  test::Rng rng{2026};
  std::vector<Command> stream;
  OrderId next_id = 1;
  for (int i = 0; i < 150; ++i) {
    if (rng.percent(20) && next_id > 1) {
      stream.push_back(CancelOrder{.id = static_cast<OrderId>(rng.uniform(1, static_cast<std::int64_t>(next_id)))});
    } else {
      stream.push_back(NewOrder{.id = next_id++,
                                .side = rng.side(),
                                .price = rng.uniform(40, 60),
                                .quantity = rng.uniform(1, 30)});
    }
  }
  SweepResult total;
  for (std::size_t i = 0; i < stream.size(); ++i) {
    const std::vector<Command> prefix(stream.begin(), stream.begin() + static_cast<std::ptrdiff_t>(i));
    for (const Buffer buffer : {Buffer::Empty, Buffer::PreReserved}) {
      const auto r = sweep(prefix, stream[i], buffer, "stream command " + std::to_string(i));
      total.points += r.points;
      total.prepare += r.prepare;
      total.residual += r.residual;
    }
    if (::testing::Test::HasFailure()) {
      break;
    }
  }
  std::printf("[alloc] random stream: %zu commands x 2 buffer modes, %ld injection points: "
              "%ld PREPARE (strong), %ld RESIDUAL (basic + audit)\n",
              stream.size(), total.points, total.prepare, total.residual);
  EXPECT_GT(total.prepare, 0);
  EXPECT_GT(total.residual, 0);
}

}  // namespace
}  // namespace lob
