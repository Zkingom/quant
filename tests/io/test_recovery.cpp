// Recovery: snapshot (optional) + journal -> last commit point (DESIGN §6.5).

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "io_test_util.hpp"
#include "lob/io/journal.hpp"
#include "lob/io/recovery.hpp"
#include "lob/io/snapshot_file.hpp"
#include "lob/testkit/rng.hpp"

namespace lob::io {
namespace {

struct Recorded {
  std::string journal;
  std::vector<SnapshotFile> after;  // after[k] = committed state after k commands
};

Recorded record_run(const testkit::WorkloadProfile& profile, std::uint64_t seed, std::size_t n) {
  const auto commands = test::generate(profile, seed, n);
  MemorySink sink;
  Session session{test::fresh(profile.config), sink, SessionOptions{}};
  Recorded r;
  r.after.push_back(session.snapshot_file());
  for (const auto& c : commands) {
    const auto res = session.submit(c);
    EXPECT_EQ(res.status, SubmitStatus::Committed);
    r.after.push_back(session.snapshot_file());
  }
  r.journal = sink.data();
  return r;
}

TEST(Recovery, JournalAloneAndSnapshotPlusJournalReachTheLiveState) {
  for (const auto& profile : testkit::standard_profiles()) {
    SCOPED_TRACE(profile.name);
    const std::size_t n = 1'500;
    const Recorded r = record_run(profile, 5, n);
    const Recovered from_log = recover(r.journal, std::nullopt, std::nullopt);
    EXPECT_EQ(from_log.state, r.after[n].state);
    EXPECT_EQ(from_log.last_cmd, n);
    EXPECT_EQ(from_log.report.tail, TailState::Clean);
    ASSERT_EQ(from_log.committed.size(), n);
    for (const std::size_t k : {std::size_t{0}, std::size_t{1}, n / 2, n - 1, n}) {
      const Recovered from_snap = recover(r.journal, r.after[k], profile.config);
      EXPECT_EQ(from_snap.state, r.after[n].state) << "snapshot at " << k;
      EXPECT_EQ(from_snap.report.batches_applied, n - k);
      EXPECT_EQ(from_snap.report.snapshot_used, k > 0);
    }
  }
}

// Truncation at sampled offsets of a long journal: recovery returns exactly
// the state after the last complete batch (prefix property).
TEST(Recovery, TruncatedJournalRecoversTheCommittedPrefix) {
  const auto& profile = *testkit::find_profile("high_density");
  const Recorded r = record_run(profile, 9, 3'000);
  const JournalScan full = scan_journal(r.journal);
  std::map<std::size_t, std::size_t> batch_end;  // end offset -> cmd count
  for (const JournalBatch& b : full.batches) {
    batch_end[b.end_offset] = b.command.cmd_seq;
  }
  testkit::Rng rng{3};
  for (int i = 0; i < 400; ++i) {
    const auto cut = static_cast<std::size_t>(rng.uniform(0, static_cast<std::int64_t>(r.journal.size())));
    const Recovered rec = recover(std::string_view(r.journal).substr(0, cut), std::nullopt, profile.config);
    const auto it = batch_end.upper_bound(cut);
    const std::size_t k = it == batch_end.begin() ? 0 : std::prev(it)->second;
    ASSERT_EQ(rec.last_cmd, k) << "cut " << cut;
    ASSERT_EQ(rec.state, r.after[k].state) << "cut " << cut;
  }
}

TEST(Recovery, InconsistentSnapshotAndJournalAreRefused) {
  const auto& profile = *testkit::find_profile("mixed");
  const Recorded r = record_run(profile, 2, 400);
  const JournalScan full = scan_journal(r.journal);

  // Snapshot ahead of the journal: the journal lost committed batches.
  const std::string short_journal = r.journal.substr(0, full.batches[199].end_offset);
  EXPECT_THROW((void)recover(short_journal, r.after[300], std::nullopt), RecoveryError);
  // Snapshot from another history: same cmd watermark, different last_seq.
  SnapshotFile foreign = r.after[300];
  foreign.state.last_seq += 1;
  EXPECT_THROW((void)recover(r.journal, foreign, std::nullopt), RecoveryError);
  // Config disagreement (command file vs journal).
  EngineConfig other = profile.config;
  other.max_live_orders += 1;
  EXPECT_THROW((void)recover(r.journal, std::nullopt, other), RecoveryError);
  // Snapshot without a journal at all.
  EXPECT_THROW((void)recover("", r.after[10], std::nullopt), RecoveryError);
  // Nothing to take a config from.
  EXPECT_THROW((void)recover("", std::nullopt, std::nullopt), RecoveryError);
}

// Batches whose checksum is valid but whose events contradict their command
// or the state: the fold reports corruption instead of inventing state.
TEST(Recovery, SemanticallyInconsistentBatchesAreCorruption) {
  const EngineConfig config{.min_price = 1, .max_price = 100, .max_order_quantity = 50, .max_live_orders = 10};
  const std::string header = encode_journal_header(config);
  const SequencedCommand rest1{.cmd_seq = 1, .command = NewOrder{.id = 1, .side = Side::Sell, .price = 50, .quantity = 5}};
  const std::string batch1 = encode_batch(rest1, std::vector<Event>{
      OrderAccepted{.seq = 1, .id = 1, .side = Side::Sell, .price = 50, .quantity = 5},
      OrderRested{.seq = 2, .id = 1, .side = Side::Sell, .price = 50, .remaining = 5}});
  const SequencedCommand buy2{.cmd_seq = 2, .command = NewOrder{.id = 2, .side = Side::Buy, .price = 50, .quantity = 3}};
  struct Case {
    std::string name;
    std::string batch2;
    std::string needle;
  };
  const std::vector<Case> cases{
      {"accepted fields differ from the command",
       encode_batch(buy2, std::vector<Event>{OrderAccepted{.seq = 3, .id = 2, .side = Side::Buy, .price = 51, .quantity = 3},
                                              OrderRested{.seq = 4, .id = 2, .side = Side::Buy, .price = 51, .remaining = 3}}),
       "first event must be the ACCEPTED"},
      {"trade with a maker that is not resting",
       encode_batch(buy2, std::vector<Event>{
                              OrderAccepted{.seq = 3, .id = 2, .side = Side::Buy, .price = 50, .quantity = 3},
                              TradeExecuted{.seq = 4, .taker_id = 2, .maker_id = 9, .taker_side = Side::Buy, .price = 50,
                                            .quantity = 3, .taker_remaining = 0, .maker_remaining = 2}}),
       "TRADE inconsistent"},
      {"wrong remaining after a trade",
       encode_batch(buy2, std::vector<Event>{
                              OrderAccepted{.seq = 3, .id = 2, .side = Side::Buy, .price = 50, .quantity = 3},
                              TradeExecuted{.seq = 4, .taker_id = 2, .maker_id = 1, .taker_side = Side::Buy, .price = 50,
                                            .quantity = 3, .taker_remaining = 0, .maker_remaining = 3}}),
       "TRADE remaining quantities inconsistent"},
      {"rested although completely filled",
       encode_batch(buy2, std::vector<Event>{
                              OrderAccepted{.seq = 3, .id = 2, .side = Side::Buy, .price = 50, .quantity = 3},
                              TradeExecuted{.seq = 4, .taker_id = 2, .maker_id = 1, .taker_side = Side::Buy, .price = 50,
                                            .quantity = 3, .taker_remaining = 0, .maker_remaining = 2},
                              OrderRested{.seq = 5, .id = 2, .side = Side::Buy, .price = 50, .remaining = 1}}),
       "events after the order was completely filled"},
      {"cancel of an order that is not resting",
       encode_batch(SequencedCommand{.cmd_seq = 2, .command = CancelOrder{.id = 7}},
                    std::vector<Event>{OrderCancelled{.seq = 3, .id = 7, .side = Side::Buy, .price = 10,
                                                      .remaining = 1, .reason = CancelReason::Requested}}),
       "CANCELLED does not match a resting order"},
  };
  ASSERT_EQ(recover(header + batch1, std::nullopt, std::nullopt).state.orders.size(), 1U);
  for (const Case& c : cases) {
    try {
      (void)recover(header + batch1 + c.batch2, std::nullopt, std::nullopt);
      ADD_FAILURE() << "accepted: " << c.name;
    } catch (const JournalCorruption& e) {
      EXPECT_NE(std::string(e.what()).find(c.needle), std::string::npos) << c.name << ": " << e.what();
    }
  }
}

}  // namespace
}  // namespace lob::io
