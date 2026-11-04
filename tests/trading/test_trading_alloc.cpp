// L3' stage 3: the strategy and the pre-trade risk gate allocate nothing.
// (The simulated exchange runs on the L1 MatchingEngine, which allocates a
// node per resting order, D-007; it is outside this guarantee -- D-046.)

#include <gtest/gtest.h>

#include "lob/risk/risk.hpp"
#include "lob/strategy/strategy.hpp"
#include "lob/testkit/alloc_guard.hpp"
#include "lob/testkit/rng.hpp"

namespace {

namespace guard = lob::testkit::alloc_guard;
namespace st = lob::strategy;

TEST(TradingAllocation, StrategyAndRiskAllocateNothing) {
  st::MarketMaker mm{};
  lob::risk::RiskLimits lim;
  lim.refill_per_second = 1'000'000;
  lim.max_rejects_in_window = 1'000'000;  // random tops: do not trip the storm switch
  lob::risk::RiskGate risk{lim};
  lob::testkit::Rng rng{5};
  st::Actions out;
  std::uint64_t accepted = 0;
  guard::reset();
  guard::arm_this_thread();
  for (std::uint64_t i = 0; i < 50000; ++i) {
    const auto bid = static_cast<std::uint32_t>(rng.uniform(9990, 10000)) * 100;
    const bool live = !rng.percent(1);
    const bool tradable = live && !rng.percent(2);
    const st::BookTop t{i, i * 1000, bid, bid + static_cast<std::uint32_t>(rng.uniform(1, 20)) * 100, 100, 100,
                        live, tradable};
    out.clear();
    mm.book_update(t, out);
    for (std::size_t k = 0; k < out.size(); ++k) {
      if (out[k].kind != st::OrderRequest::Kind::New) continue;
      const auto v = risk.check_new(out[k].side, out[k].price, out[k].qty, {}, {t.bid_price, t.ask_price, t.live, t.tradable},
                                    t.ts);
      accepted += v == lob::risk::Verdict::Accept ? 1U : 0U;
      if (rng.percent(20)) {
        st::Actions more;
        mm.fill(st::Fill{out[k].client_id, out[k].side, out[k].price, 50, 50, t.ts}, more);
      }
    }
    if (i % 997 == 0) mm.timer(t.ts, out);
  }
  guard::disarm_this_thread();
  EXPECT_EQ(guard::violations(), 0) << guard::violation_bytes() << " bytes";
  EXPECT_GT(accepted, 1000U);
}

}  // namespace
