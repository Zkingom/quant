// Market-wide mode without allocation on the hot path (D-058): instruments
// pre-created from a measured profile, level arrays reserved and capped,
// broadcast log reserved. lob_testkit_alloc_guard replaces operator new;
// every pipeline thread arms the guard for its hot loop.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lob/pipeline/market_pipeline.hpp"
#include "lob/testkit/alloc_guard.hpp"
#include "lob/testkit/itch_builder.hpp"
#include "lob/testkit/itch_generator.hpp"

namespace {

namespace md = lob::md;
namespace pl = lob::pipeline;
namespace tk = lob::testkit::itch;
namespace guard = lob::testkit::alloc_guard;

void arm() { guard::arm_this_thread(); }
void disarm() { guard::disarm_this_thread(); }

tk::Buf stream() {
  tk::GeneratorParams gp;
  gp.seed = 31;
  gp.messages = 40000;
  gp.instruments = 11;
  gp.max_live_per_instrument = 200;
  return tk::generate_stream(gp);
}

md::MarketConfig base() {
  md::MarketConfig c;
  c.max_orders_per_shard = 1U << 14;
  c.max_levels_per_side = 1024;
  c.initial_levels_per_side = 4;  // on-the-fly instruments start small and grow
  return c;
}

pl::MarketRun run(const tk::Buf& s, const md::MarketConfig& c, std::size_t shards, bool hooks) {
  pl::MemorySource src{s, 5000};
  pl::RunConfig rc;
  rc.chunk_bytes = 1U << 17;
  rc.queue_capacity = 256;
  if (hooks) rc.hooks = pl::ThreadHooks{&arm, &disarm};
  return pl::run_market(src, c, shards, rc);
}

/// The "measured profile": every instrument's peak level count, margin 2.
md::MarketConfig profiled(const pl::MarketRun& measured, double margin = 2.0) {
  md::MarketConfig c = base();
  for (const auto& s : measured.summary.stocks) {
    c.instruments.push_back(md::InstrumentSpec{
        s.locate, md::make_symbol(s.symbol),
        std::max<std::size_t>(16, static_cast<std::size_t>(static_cast<double>(s.peak_levels) * margin + 0.999))});
  }
  return c;
}

TEST(MarketAllocation, PreCreatedInstrumentsAllocateNothingSingleThreaded) {
  const tk::Buf s = stream();
  const auto measured = run(s, base(), 0, false);
  ASSERT_EQ(measured.summary.stocks.size(), 11U);
  guard::reset();
  const auto r = run(s, profiled(measured), 0, true);
  EXPECT_EQ(guard::violations(), 0) << guard::violation_bytes() << " bytes";
  EXPECT_EQ(r.summary.late_instruments, 0U);
  EXPECT_EQ(r.summary.summary_hash, measured.summary.summary_hash) << "pre-creation changes nothing observable";
  EXPECT_EQ(r.summary.live, 11U);
}

TEST(MarketAllocation, PreCreatedInstrumentsAllocateNothingSharded) {
  const tk::Buf s = stream();
  const auto measured = run(s, base(), 0, false);
  guard::reset();
  const auto r = run(s, profiled(measured), 3, true);
  EXPECT_EQ(guard::violations(), 0) << guard::violation_bytes() << " bytes";
  EXPECT_EQ(r.summary.summary_hash, measured.summary.summary_hash);
}

TEST(MarketAllocation, NegativeControlOnTheFlyInstrumentsDoAllocate) {
  const tk::Buf s = stream();
  guard::reset();
  const auto r = run(s, base(), 0, true);
  EXPECT_GT(guard::violations(), 11) << "handlers created at their Stock Directory, level arrays grow";
  EXPECT_EQ(r.summary.late_instruments, 11U);
}

TEST(MarketAllocation, AReserveTooSmallIsALevelCapacityNotAnAllocation) {
  const tk::Buf s = stream();
  const auto measured = run(s, base(), 0, false);
  md::MarketConfig c = profiled(measured);
  for (auto& i : c.instruments) {
    if (i.symbol == md::make_symbol("SYM3")) i.levels = 2;  // far below its measured peak
  }
  guard::reset();
  const auto r = run(s, c, 0, true);
  EXPECT_EQ(guard::violations(), 0);
  for (const auto& st : r.summary.stocks) {
    if (st.symbol == "SYM3") {
      EXPECT_EQ(st.state, md::FeedState::Stale);
      EXPECT_EQ(st.stale_reason, md::AlertReason::BookRejected);
    } else {
      EXPECT_EQ(st.state, md::FeedState::Live) << st.symbol;
    }
  }
}

}  // namespace
