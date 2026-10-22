// Allocation-failure acceptance tests for the recovery system: the M4
// contracts C1–C4 restated against the real Session / journal / recovery
// (DESIGN.md §6.2 Q8). Separate executable: it replaces global operator new.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <limits>
#include <new>
#include <string>
#include <vector>

#include "io_test_util.hpp"
#include "lob/io/journal.hpp"
#include "lob/io/recovery.hpp"
#include "lob/io/session.hpp"
#include "lob/io/verify.hpp"
#include "lob/testkit/alloc_fault.hpp"
#include "lob/testkit/describe.hpp"
#include "lob/testkit/rng.hpp"

namespace lob::io {
namespace {

using testkit::alloc::FaultWindow;

bool ends_with_resource_cancel(const JournalBatch& b) {
  const auto* c = std::get_if<OrderCancelled>(&b.events.back());
  return c != nullptr && c->reason == CancelReason::InsufficientResources;
}

// For every command of a stream and every allocation index k inside its
// submit(): exactly one of the contract outcomes, each checked:
//   Committed       -> journal identical to the failure-free journal (C1: a
//                      prepare failure was retried with no trace);
//   CommittedWithResourceFailure -> batch ends INSUFFICIENT_RESOURCES; journal
//                      recovery == session state (C4); re-submission is
//                      DUPLICATE (C2); verify takes that outcome from the
//                      journal and finds no divergence (C3);
//   Poisoned        -> journal unchanged; recovery == state before the command;
//   NotApplied      -> journal unchanged; a plain retry commits the
//                      failure-free batch (C1 at the caller).
//
// k runs from 0 until the window no longer injects, i.e. past the LAST
// allocation submit() makes for that command — including any allocation
// after the commit point (an exception escaping submit() fails the test).
// An earlier version swept only k < 8, never reached the post-commit code
// (submit allocates dozens of times, mostly in encode_batch), and so did not
// detect the "commit record allocates after the commit point" mutation.
TEST(IoAllocFailure, EveryCommandAndAllocationPointHasAContractOutcome) {
  const auto& profile = *testkit::find_profile("mixed");
  const auto commands = test::generate(profile, 21, 80);
  MemorySink reference;
  test::run_all(profile.config, commands, reference);
  const JournalScan ref_scan = scan_journal(reference.data());
  auto ref_prefix = [&](std::size_t batches) {
    return reference.data().substr(0, batches == 0 ? encode_journal_header(profile.config).size()
                                                   : ref_scan.batches[batches - 1].end_offset);
  };

  long committed_after_injection = 0, resource = 0, poisoned = 0, not_applied = 0, points = 0;
  long max_points_per_command = 0;
  for (std::size_t i = 0; i < commands.size(); ++i) {
    bool reached_the_end = false;
    for (long k = 0; k < 1'000 && !reached_the_end; ++k) {
      SCOPED_TRACE("command " + std::to_string(i + 1) + " k=" + std::to_string(k));
      MemorySink sink;
      Session s{test::fresh(profile.config), sink, SessionOptions{.durable = false, .prepare_retries = 2}};
      for (std::size_t j = 0; j < i; ++j) {
        ASSERT_EQ(s.submit(commands[j]).status, SubmitStatus::Committed);
      }
      const std::string before = sink.data();
      SubmitResult r;
      bool injected = false;
      {
        FaultWindow window{k};
        r = s.submit(commands[i]);
        injected = window.injected();
      }
      if (!injected) {
        reached_the_end = true;  // k is past the last allocation of this command
        max_points_per_command = std::max(max_points_per_command, k);
      } else {
        ++points;
      }
      switch (r.status) {
        case SubmitStatus::Committed:
          ASSERT_EQ(sink.data(), ref_prefix(i + 1));
          committed_after_injection += injected ? 1 : 0;
          break;
        case SubmitStatus::CommittedWithResourceFailure: {
          ++resource;
          const JournalScan scan = scan_journal(sink.data());
          ASSERT_TRUE(ends_with_resource_cancel(scan.batches.back()));
          ASSERT_EQ(recover(sink.data(), std::nullopt, std::nullopt).state, s.snapshot_file().state);
          ASSERT_EQ(s.submit(commands[i]).status, SubmitStatus::Duplicate);
          const VerifyResult v = verify_journal(scan);
          ASSERT_EQ(v.divergence, std::nullopt);
          ASSERT_EQ(v.outcome_from_journal, 1U);
          break;
        }
        case SubmitStatus::Poisoned: {
          ++poisoned;
          ASSERT_EQ(sink.data(), before);
          const Recovered rec = recover(sink.data(), std::nullopt, profile.config);
          ASSERT_EQ(rec.last_cmd, i);
          ASSERT_EQ(rec.state, recover(ref_prefix(i), std::nullopt, profile.config).state);
          break;
        }
        case SubmitStatus::NotApplied:
          ++not_applied;
          ASSERT_EQ(sink.data(), before);
          ASSERT_EQ(s.submit(commands[i]).status, SubmitStatus::Committed);
          ASSERT_EQ(sink.data(), ref_prefix(i + 1));
          break;
        default:
          FAIL() << "unexpected outcome " << to_string(r.status);
      }
    }
  }
  std::printf("[io-alloc] %zu commands, %ld injection points (max %ld per command): committed after an "
              "injected failure=%ld resource_failure=%ld poisoned=%ld not_applied=%ld\n",
              commands.size(), points, max_points_per_command, committed_after_injection, resource, poisoned,
              not_applied);
  EXPECT_GT(committed_after_injection, 0);
  EXPECT_GT(resource, 0);
  EXPECT_GT(poisoned, 0);
}

// Long faulted runs on every profile, restarting after every poisoning
// exactly as a crashed process would (recover from the journal bytes,
// truncate, re-submit from the failed command). At the end:
//   - the journal recovers to exactly the live session state (C4);
//   - re-execution agrees with the journal except at resource-failure
//     batches, whose outcome is taken from the journal (C3);
//   - an engine restored from the recovered state and the live session
//     produce identical events for the next commands (recovery is exact).
TEST(IoAllocFailure, LongFaultedRunsWithRestartsRecoverExactly) {
  long restarts = 0, resource_batches = 0, retries = 0;
  for (const auto& profile : testkit::standard_profiles()) {
    SCOPED_TRACE(profile.name);
    const auto commands = test::generate(profile, 31, 2'300);
    const std::size_t faulted = 2'000;
    testkit::Rng rng{97};
    auto sink = std::make_unique<MemorySink>();
    auto session = std::make_unique<Session>(test::fresh(profile.config), *sink, SessionOptions{});
    std::size_t i = 0;
    while (i < faulted) {
      SubmitResult r;
      if (rng.percent(3)) {
        FaultWindow window{rng.uniform(0, 7)};
        r = session->submit(commands[i]);
      } else {
        r = session->submit(commands[i]);
      }
      switch (r.status) {
        case SubmitStatus::Committed:
          ++i;
          break;
        case SubmitStatus::CommittedWithResourceFailure:
          ++resource_batches;
          ++i;
          break;
        case SubmitStatus::NotApplied:
          ++retries;  // try the same command again
          break;
        case SubmitStatus::Poisoned: {
          ++restarts;  // "process restart": recover, truncate, continue
          const Recovered rec = recover(sink->data(), std::nullopt, profile.config);
          ASSERT_EQ(rec.last_cmd, i);
          auto next = std::make_unique<MemorySink>();
          next->write(std::string_view(sink->data()).substr(0, rec.report.valid_bytes));
          session = std::make_unique<Session>(rec, *next, SessionOptions{});
          sink = std::move(next);
          break;
        }
        default:
          FAIL() << "unexpected outcome " << to_string(r.status) << " at command " << i + 1;
      }
    }
    const Recovered rec = recover(sink->data(), std::nullopt, std::nullopt);
    ASSERT_EQ(rec.state, session->snapshot_file().state);
    ASSERT_EQ(rec.last_cmd, faulted);
    const VerifyResult v = verify_journal(scan_journal(sink->data()));
    ASSERT_EQ(v.divergence, std::nullopt);
    std::size_t rf_in_journal = 0;
    for (const auto& c : rec.committed) {
      rf_in_journal += c.resource_failure ? 1 : 0;
    }
    ASSERT_EQ(v.outcome_from_journal, rf_in_journal);

    MatchingEngine restored = MatchingEngine::restore(rec.state);
    std::vector<Event> events;
    for (std::size_t j = faulted; j < commands.size(); ++j) {
      events.clear();
      restored.process(commands[j].command, events);
      ASSERT_EQ(session->submit(commands[j]).status, SubmitStatus::Committed);
      const auto live = session->last_events();
      ASSERT_TRUE(std::equal(events.begin(), events.end(), live.begin(), live.end()))
          << "command " << j + 1 << '\n' << testkit::describe(events);
    }
  }
  std::printf("[io-alloc] long runs, 10 profiles x 2000 faulted commands: restarts=%ld "
              "resource_failure_batches=%ld not_applied_retries=%ld\n",
              restarts, resource_batches, retries);
  EXPECT_GT(restarts, 0);
  EXPECT_GT(resource_batches, 0);
}

// R11: sequence exhaustion is refused before any effect and leaves the
// session usable (reachable without white-box access via a restored state).
TEST(IoAllocFailure, SequenceExhaustionIsRefusedWithoutEffect) {
  const EngineConfig config{.min_price = 1, .max_price = 100, .max_order_quantity = 10, .max_live_orders = 10};
  Recovered rec = test::fresh(config);
  rec.state.last_seq = std::numeric_limits<SeqNum>::max() - 1;
  MemorySink sink;
  Session s{rec, sink, SessionOptions{}};
  const std::string header = sink.data();
  const SubmitResult refused =
      s.submit(SequencedCommand{.cmd_seq = 1, .command = NewOrder{.id = 1, .side = Side::Buy, .price = 5, .quantity = 1}});
  EXPECT_EQ(refused.status, SubmitStatus::Refused);
  EXPECT_FALSE(s.poisoned());
  EXPECT_EQ(sink.data(), header);
  const SubmitResult last =
      s.submit(SequencedCommand{.cmd_seq = 1, .command = CancelOrder{.id = 3}});  // needs one seq: fits
  EXPECT_EQ(last.status, SubmitStatus::Committed);
  EXPECT_EQ(last.last_seq, std::numeric_limits<SeqNum>::max());
}

// M6 E1: the commit record reserved before each command grows
// geometrically. An exact reserve(size + 1) re-copies every earlier record on
// every submit — about 32 * n^2 / 2 bytes over n commands, ~256 MB for
// n = 4000, against a few MB of ordinary per-command allocations. Measured
// with this test (gcc-release): 260.9 MB with the exact reserve, 5.1 MB
// with geometric growth.
TEST(IoAllocFailure, CommitRecordStorageDoesNotGrowQuadratically) {
  const auto& profile = *testkit::find_profile("mixed");
  const auto commands = test::generate(profile, 5, 4000);
  MemorySink sink;
  Session s{test::fresh(profile.config), sink, SessionOptions{.durable = false, .prepare_retries = 2}};
  long long bytes = 0;
  {
    FaultWindow window{std::numeric_limits<long>::max()};  // count only, never inject
    for (const auto& c : commands) {
      ASSERT_EQ(s.submit(c).status, SubmitStatus::Committed);
    }
    bytes = testkit::alloc::allocated_bytes();
    EXPECT_FALSE(window.injected());
  }
  std::printf("bytes allocated by 4000 submits: %lld\n", bytes);
  EXPECT_LT(bytes, 64LL << 20);
}

}  // namespace
}  // namespace lob::io
