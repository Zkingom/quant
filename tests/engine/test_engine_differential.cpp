// Differential tests: MatchingEngine vs. ReferenceEngine + EventAuditor,
// driven by lob::testkit (generator, runner and checks live there; see
// lob/testkit/differential.hpp for what is compared after every command).

#include <gtest/gtest.h>

#include <cstdio>
#include <string>

#include "lob/testkit/differential.hpp"
#include "lob/testkit/workload.hpp"

namespace lob {
namespace {

using testkit::Coverage;
using testkit::RunOptions;
using testkit::RunReport;

Coverage run_seeds(const testkit::WorkloadProfile& profile, int seeds, long commands,
                   long check_every) {
  Coverage merged;
  for (int seed = 1; seed <= seeds; ++seed) {
    RunOptions options;
    options.seed = static_cast<std::uint64_t>(seed);
    options.commands = commands;
    options.full_check_every = check_every;
    const RunReport report = testkit::run_differential(profile, options);
    EXPECT_TRUE(report.ok) << "profile " << profile.name << " seed " << seed << '\n' << report.failure;
    if (!report.ok) {
      break;
    }
    merged.merge(report.coverage);
  }
  return merged;
}

// The M3 differential workload, now produced by the testkit generator with
// the "mixed" profile, must be the SAME stream: every counter below is the
// value recorded at M3 acceptance (MODULE_NOTES/evidence/M3/
// differential_and_alloc.log). Any difference means the M4 refactoring
// changed what is tested. Changing the generator on purpose requires
// updating these numbers in the same commit, with a reason.
TEST(EngineDifferential, MixedProfileReproducesTheM3Stream) {
  const auto* mixed = testkit::find_profile("mixed");
  ASSERT_NE(mixed, nullptr);
  const Coverage c = run_seeds(*mixed, 20, 5'000, 1);
  std::printf("%s", c.summary().c_str());

  EXPECT_EQ(c.commands, 100'000);
  EXPECT_EQ(c.accepted, 68'558);
  EXPECT_EQ(c.trades, 54'005);
  EXPECT_EQ(c.cancels, 13'555);
  EXPECT_EQ(c.multi_maker_commands, 12'191);
  EXPECT_EQ(c.multi_level_commands, 8'145);
  EXPECT_EQ(c.taker_fully_filled, 19'829);
  EXPECT_EQ(c.taker_partial_then_rested, 5'924);
  EXPECT_EQ(c.rested_without_trade, 42'805);
  EXPECT_EQ(c.maker_partially_filled, 18'970);
  EXPECT_EQ(c.accepted_while_full, 8'518);
  EXPECT_EQ(c.reused_dead_id, 3'601);
  EXPECT_EQ(c.max_orders, 40U);
  EXPECT_EQ(c.reject_count(RejectReason::InvalidOrderId), 1'203);
  EXPECT_EQ(c.reject_count(RejectReason::InvalidSide), 1'200);
  EXPECT_EQ(c.reject_count(RejectReason::PriceOutOfRange), 1'273);
  EXPECT_EQ(c.reject_count(RejectReason::InvalidQuantity), 1'189);
  EXPECT_EQ(c.reject_count(RejectReason::QuantityTooLarge), 1'213);
  EXPECT_EQ(c.reject_count(RejectReason::DuplicateOrderId), 136);
  EXPECT_EQ(c.reject_count(RejectReason::UnknownOrderId), 5'571);
  EXPECT_EQ(c.reject_count(RejectReason::BookFull), 6'102);
  EXPECT_EQ(c.full_checks, 100'000 + 20);  // every command + once at the end of each seed

  EXPECT_TRUE(testkit::unmet_targets(*mixed, c).empty());
}

// The repro procedure (DESIGN §4.4) relies on this: sparse full checks must
// not change the command stream, only when a divergence is detected.
TEST(EngineDifferential, CheckIntervalDoesNotChangeTheStream) {
  for (const auto& profile : testkit::standard_profiles()) {
    SCOPED_TRACE(profile.name);
    const Coverage every = run_seeds(profile, 1, 2'000, 1);
    const Coverage sparse = run_seeds(profile, 1, 2'000, 997);
    EXPECT_EQ(every.commands, sparse.commands);
    EXPECT_EQ(every.accepted, sparse.accepted);
    EXPECT_EQ(every.trades, sparse.trades);
    EXPECT_EQ(every.cancels, sparse.cancels);
    EXPECT_EQ(every.reused_dead_id, sparse.reused_dead_id);
    EXPECT_EQ(every.side_emptied, sparse.side_emptied);
    EXPECT_TRUE(every.accepted_quantity == sparse.accepted_quantity);
    for (int r = 0; r < 8; ++r) {
      EXPECT_EQ(every.rejects[r], sparse.rejects[r]) << r;
    }
    EXPECT_GT(every.full_checks, sparse.full_checks);
  }
}

}  // namespace
}  // namespace lob
