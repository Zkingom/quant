// L3' stage 3: market-maker strategy, CRTP vs virtual equivalence, the two
// fill models of the simulated exchange (on the unchanged L1 engine), the
// trading stage (kill switch on a stale book), and the consistency gate:
// file replay and the multi-threaded pipeline produce identical orders,
// fills and positions. Label "concurrency".

#include "lob/sim/trading.hpp"

#include <gtest/gtest.h>

#include <vector>

#include "lob/testkit/itch_builder.hpp"
#include "lob/testkit/itch_generator.hpp"

namespace {

namespace md = lob::md;
namespace pl = lob::pipeline;
namespace sim = lob::sim;
namespace st = lob::strategy;
namespace tk = lob::testkit::itch;
using Side = md::itch::Side;
using st::OrderRequest;

st::BookTop top(std::uint32_t bid, std::uint32_t ask, bool live = true) {
  return st::BookTop{1, 1, bid, ask, 100, 100, live};
}

// ---------------------------------------------------------------- strategy

TEST(MarketMaker, QuotesAroundTheMidWithInventorySkew) {
  st::MarketMakerParams p;
  p.tick = 100;
  p.half_spread = 200;
  p.skew_per_share = 2;
  st::MarketMaker mm{p};
  st::Actions out;
  mm.book_update(top(999'000, 1'001'000), out);  // mid 1 000 000
  ASSERT_EQ(out.size(), 2U);
  EXPECT_EQ(out[0].kind, OrderRequest::Kind::New);
  EXPECT_EQ(out[0].side, Side::Buy);
  EXPECT_EQ(out[0].price, 999'800U);
  EXPECT_EQ(out[1].side, Side::Sell);
  EXPECT_EQ(out[1].price, 1'000'200U);
  // Long 150 shares: reservation price moves down by 300 -> both quotes lower.
  out.clear();
  mm.fill(st::Fill{1, Side::Buy, 999'800, 100, 0, 2}, out);  // our bid (id 1), completely filled
  mm.fill(st::Fill{99, Side::Buy, 999'800, 50, 0, 2}, out);
  EXPECT_EQ(mm.inventory(), 150);
  out.clear();
  mm.book_update(top(999'000, 1'001'000), out);
  // bid was filled (id 1 cleared) -> new bid; ask requoted (cancel + new).
  std::uint32_t bid = 0, ask = 0;
  for (std::size_t i = 0; i < out.size(); ++i) {
    if (out[i].kind != OrderRequest::Kind::New) continue;
    (out[i].side == Side::Buy ? bid : ask) = out[i].price;
  }
  EXPECT_EQ(bid, 999'500U);   // 1 000 000 - 300 - 200
  EXPECT_EQ(ask, 999'900U);    // 1 000 000 - 300 + 200, already on a tick
}

TEST(MarketMaker, RequotesOnlyWhenThePriceChanges) {
  st::MarketMaker mm{};
  st::Actions out;
  mm.book_update(top(999'000, 1'001'000), out);
  EXPECT_EQ(out.size(), 2U);
  out.clear();
  mm.book_update(top(999'000, 1'001'000), out);
  EXPECT_EQ(out.size(), 0U);
}

TEST(MarketMaker, StaysPassiveInsideATightSpread) {
  st::MarketMaker mm{};
  st::Actions out;
  mm.book_update(top(1'000'000, 1'000'100), out);  // one-tick spread
  ASSERT_EQ(out.size(), 2U);
  EXPECT_LT(out[0].price, 1'000'100U);
  EXPECT_GT(out[1].price, 1'000'000U);
}

TEST(MarketMaker, StopsQuotingASideAtMaxInventory) {
  st::MarketMakerParams p;
  p.max_inventory = 100;
  st::MarketMaker mm{p};
  st::Actions out;
  mm.fill(st::Fill{7, Side::Buy, 1, 100, 0, 1}, out);
  mm.book_update(top(999'000, 1'001'000), out);
  ASSERT_EQ(out.size(), 1U);
  EXPECT_EQ(out[0].side, Side::Sell);
}

TEST(MarketMaker, CancelsEverythingWhenTheBookIsNotLive) {
  st::MarketMaker mm{};
  st::Actions out;
  mm.book_update(top(999'000, 1'001'000), out);
  out.clear();
  mm.book_update(top(999'000, 1'001'000, false), out);
  ASSERT_EQ(out.size(), 2U);
  EXPECT_EQ(out[0].kind, OrderRequest::Kind::Cancel);
  EXPECT_EQ(out[1].kind, OrderRequest::Kind::Cancel);
}

TEST(MarketMaker, CrtpAndVirtualDispatchProduceIdenticalRequests) {
  st::MarketMaker crtp{};
  auto virt = st::make_virtual_market_maker({});
  lob::testkit::Rng rng{3};
  for (int i = 0; i < 5000; ++i) {
    const auto bid = static_cast<std::uint32_t>(rng.uniform(990, 1000)) * 1000;
    const st::BookTop t = top(bid, bid + static_cast<std::uint32_t>(rng.uniform(1, 30)) * 100, !rng.percent(2));
    st::Actions a, b;
    crtp.book_update(t, a);
    virt->book_update(t, b);
    if (rng.percent(10) && a.size() > 0) {
      const st::Fill f{a[a.size() - 1].client_id, a[a.size() - 1].side, a[a.size() - 1].price, 10, 90, 1};
      crtp.fill(f, a);
      virt->fill(f, b);
    }
    ASSERT_EQ(a.size(), b.size()) << i;
    for (std::size_t k = 0; k < a.size(); ++k) {
      ASSERT_EQ(a[k].kind, b[k].kind);
      ASSERT_EQ(a[k].client_id, b[k].client_id);
      ASSERT_EQ(a[k].price, b[k].price);
    }
  }
}

// ---------------------------------------------------------------- fill models

/// A tiny market: an md::Book with resting market orders, and helpers that
/// build the BookUpdate events the feed handler would publish.
struct Market {
  md::Book book{md::BookConfig{64, 16}};
  md::FeedEvent exec(std::uint64_t ref, std::uint32_t qty) {
    const auto o = *book.find(ref);
    md::FeedEvent e{};
    e.kind = md::FeedEvent::Kind::BookUpdate;
    e.msg_type = 'E';
    e.order_ref = ref;
    e.side = static_cast<char>(o.side);
    e.price = o.price;
    e.shares = qty;
    EXPECT_EQ(book.execute(ref, qty), md::BookStatus::Applied);
    return e;
  }
  md::FeedEvent cancel(std::uint64_t ref, std::uint32_t qty) {
    const auto o = *book.find(ref);
    md::FeedEvent e{};
    e.kind = md::FeedEvent::Kind::BookUpdate;
    e.msg_type = 'X';
    e.order_ref = ref;
    e.side = static_cast<char>(o.side);
    e.price = o.price;
    e.shares = qty;
    EXPECT_EQ(book.cancel(ref, qty), md::BookStatus::Applied);
    return e;
  }
};

std::uint64_t filled(const sim::Reports& r, std::uint64_t id) {
  std::uint64_t q = 0;
  for (std::size_t i = 0; i < r.size(); ++i) {
    if (r[i].kind == sim::Report::Kind::Fill && r[i].client_id == id) q += r[i].qty;
  }
  return q;
}

class FillModels : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(m.book.add(10, Side::Sell, 300, 10100), md::BookStatus::Applied);  // ahead of us
    ASSERT_EQ(m.book.add(11, Side::Buy, 100, 9900), md::BookStatus::Applied);
    for (sim::SimExchange* x : {&opt, &cons}) {
      sim::Reports r;
      x->submit(OrderRequest{OrderRequest::Kind::New, 1, Side::Sell, 10100, 100}, m.book, r);
      ASSERT_EQ(r.size(), 0U) << "rests without a report";
    }
    ASSERT_EQ(m.book.add(12, Side::Sell, 500, 10100), md::BookStatus::Applied);  // behind us
  }
  Market m;
  sim::SimExchange opt{sim::SimConfig{sim::FillModel::Optimistic, {}}};
  sim::SimExchange cons{sim::SimConfig{sim::FillModel::Conservative, {}}};
};

TEST_F(FillModels, OptimisticAssumesTheFrontOfTheQueue) {
  sim::Reports r;
  opt.on_market(m.exec(10, 50), r);
  EXPECT_EQ(filled(r, 1), 50U);
  ASSERT_EQ(r.size(), 1U);
  EXPECT_EQ(r[0].price, 10100U) << "maker (our) price";
  EXPECT_EQ(r[0].leaves, 50U);
  EXPECT_EQ(opt.engine().book().order_count(), 1U) << "the synthetic remainder never rests";
}

TEST_F(FillModels, ConservativeWaitsForTheQueueAhead) {
  sim::Reports r;
  cons.on_market(m.exec(10, 250), r);  // 300 ahead -> 50 ahead
  EXPECT_EQ(filled(r, 1), 0U);
  cons.on_market(m.exec(10, 50), r);   // queue ahead exhausted exactly
  EXPECT_EQ(filled(r, 1), 0U);
  cons.on_market(m.exec(12, 70), r);   // now executions reach us
  EXPECT_EQ(filled(r, 1), 70U);
  EXPECT_EQ(cons.engine().book().order_count(), 1U);
}

TEST_F(FillModels, CancelsAheadShrinkTheQueueButCancelsBehindDoNot) {
  sim::Reports r;
  cons.on_market(m.cancel(12, 400), r);  // behind us: no effect
  cons.on_market(m.exec(10, 299), r);
  EXPECT_EQ(filled(r, 1), 0U);
  cons.on_market(m.cancel(10, 1), r);    // the last share ahead leaves
  cons.on_market(m.exec(12, 30), r);
  EXPECT_EQ(filled(r, 1), 30U);
}

TEST_F(FillModels, TradeThroughOurPriceFillsInBothModels) {
  ASSERT_EQ(m.book.add(13, Side::Sell, 1000, 10200), md::BookStatus::Applied);
  // The market traded a sell resting at 10200: a buyer paid more than our 10100.
  md::FeedEvent e{};
  e.kind = md::FeedEvent::Kind::BookUpdate;
  e.msg_type = 'E';
  e.side = 'S';
  e.price = 10200;
  e.shares = 1000;
  e.order_ref = 13;
  sim::Reports a, b;
  opt.on_market(e, a);
  cons.on_market(e, b);
  EXPECT_EQ(filled(a, 1), 100U);
  EXPECT_EQ(filled(b, 1), 100U);
}

TEST_F(FillModels, TradeThroughFillsAtMostTheExecutedShares) {
  ASSERT_EQ(m.book.add(13, Side::Sell, 1000, 10200), md::BookStatus::Applied);
  md::FeedEvent e{};
  e.kind = md::FeedEvent::Kind::BookUpdate;
  e.msg_type = 'E';
  e.side = 'S';
  e.price = 10200;
  e.shares = 30;
  e.order_ref = 13;
  sim::Reports a, b;
  opt.on_market(e, a);
  cons.on_market(e, b);
  EXPECT_EQ(filled(a, 1), 30U);
  EXPECT_EQ(filled(b, 1), 30U);
}

TEST_F(FillModels, ExecutionsOnTheOtherSideOrWorsePricesDoNotFill) {
  sim::Reports r;
  opt.on_market(m.exec(11, 100), r);  // a resting BUY traded: sellers aggressed; our ask is not hit
  EXPECT_EQ(filled(r, 1), 0U);
}

TEST_F(FillModels, CancelOfOurOrderReportsTheRemainder) {
  sim::Reports r;
  opt.submit(OrderRequest{OrderRequest::Kind::Cancel, 1, Side::Sell, 0, 0}, m.book, r);
  ASSERT_EQ(r.size(), 1U);
  EXPECT_EQ(r[0].kind, sim::Report::Kind::Cancelled);
  EXPECT_EQ(r[0].qty, 100U);
  EXPECT_EQ(opt.engine().book().order_count(), 0U);
}

// ---------------------------------------------------------------- trading stage

md::FeedConfig feed_cfg() {
  md::FeedConfig c;
  c.symbol = md::make_symbol(tk::symbol_of(1));
  c.book = md::BookConfig{4096, 1024};
  return c;
}

sim::TradingConfig trading_cfg(sim::FillModel m) {
  sim::TradingConfig c;
  c.sim.model = m;
  c.risk.bucket_capacity = 1000;
  c.risk.refill_per_second = 1'000'000;
  c.risk.max_rejects_in_window = 1'000'000;
  c.risk.max_position = 5000;
  c.timer_interval_ns = 1'000'000;  // 1 ms of ITCH time (test streams span a few ms)
  return c;
}

struct Result {
  std::uint64_t orders, fills, position;
  std::int64_t pos, pnl;
  std::uint64_t filled_qty, n_requests;
  bool operator==(const Result&) const = default;
};

struct DualResult {
  Result opt, cons;
  std::uint64_t timers;
};

DualResult run_trading(const tk::Buf& stream, bool pipelined, std::size_t queue = 64, std::size_t max_read = SIZE_MAX) {
  st::MarketMaker mm_opt{};
  st::MarketMaker mm_cons{};
  sim::TradingStage<st::MarketMaker> opt{trading_cfg(sim::FillModel::Optimistic), mm_opt};
  sim::TradingStage<st::MarketMaker> cons{trading_cfg(sim::FillModel::Conservative), mm_cons};
  sim::Both<decltype(opt), decltype(cons)> both{opt, cons};
  pl::BookStage<decltype(both)> stage{feed_cfg(), both};
  pl::MemorySource src{stream, max_read};
  pl::RunConfig rc;
  rc.chunk_bytes = 1U << 17;
  rc.queue_capacity = queue;
  if (pipelined) pl::run_pipelined(src, stage, rc); else pl::run_single(src, stage, rc);
  auto res = [](const auto& t) {
    return Result{t.digest().orders, t.digest().fills, t.position_hash(), t.position(), t.pnl(),
                  t.digest().filled_qty, t.digest().n_requests};
  };
  return DualResult{res(opt), res(cons), mm_opt.timers()};
}

#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
constexpr std::size_t kMessages = 6000;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
constexpr std::size_t kMessages = 6000;
#else
constexpr std::size_t kMessages = 40000;
#endif
#else
constexpr std::size_t kMessages = 40000;
#endif

TEST(TradingConsistency, ReplayAndPipelineProduceIdenticalOrdersFillsAndPositions) {
  for (std::uint64_t seed = 1; seed <= 3; ++seed) {
    tk::GeneratorParams gp;
    gp.seed = seed;
    gp.messages = kMessages;
    const tk::Buf s = tk::generate_stream(gp);
    const DualResult ref = run_trading(s, false);
    ASSERT_GT(ref.opt.n_requests, 100U) << "the strategy must actually trade";
    ASSERT_GT(ref.opt.filled_qty, 0U);
    EXPECT_GT(ref.timers, 0U);
    for (const std::size_t cap : {1U, 64U, 4096U}) {
      const DualResult got = run_trading(s, true, cap, 5000);
      EXPECT_EQ(got.opt, ref.opt) << "optimistic, seed " << seed << " cap " << cap;
      EXPECT_EQ(got.cons, ref.cons) << "conservative, seed " << seed << " cap " << cap;
    }
  }
}

// The two models bound reality only for the SAME order flow (open loop).
// A strategy that reacts to its fills (the market maker skews by inventory)
// produces different orders under each model, so its totals are not
// ordered -- e.g. seed 8, 40 000 messages: optimistic 280 636 shares,
// conservative 394 800 (MODULE_NOTES/L3 stage 3). This test uses a
// strategy whose orders do not depend on fills: join the best bid and the
// best ask once, never requote.
class JoinOnce : public st::StrategyBase<JoinOnce> {
 public:
  void on_book_update(const st::BookTop& t, st::Actions& out) {
    if (placed_ || !t.live || t.bid_price == 0 || t.ask_price == 0) return;
    placed_ = true;
    out.push(OrderRequest{OrderRequest::Kind::New, 1, Side::Buy, t.bid_price, 1000});
    out.push(OrderRequest{OrderRequest::Kind::New, 2, Side::Sell, t.ask_price, 1000});
  }
  void on_fill(const st::Fill&, st::Actions&) {}
  void on_timer(std::uint64_t, st::Actions&) {}

 private:
  bool placed_{false};
};

TEST(TradingConsistency, OptimisticBoundsConservativeForTheSameOrderFlow) {
  int strictly = 0;
  for (std::uint64_t seed = 1; seed <= 6; ++seed) {
    tk::GeneratorParams gp;
    gp.seed = seed;
    gp.messages = kMessages;
    const tk::Buf s = tk::generate_stream(gp);
    JoinOnce a, b;
    sim::TradingStage<JoinOnce> opt{trading_cfg(sim::FillModel::Optimistic), a};
    sim::TradingStage<JoinOnce> cons{trading_cfg(sim::FillModel::Conservative), b};
    sim::Both<decltype(opt), decltype(cons)> both{opt, cons};
    pl::BookStage<decltype(both)> stage{feed_cfg(), both};
    pl::MemorySource src{s};
    pl::run_single(src, stage, pl::RunConfig{});
    ASSERT_EQ(opt.digest().orders, cons.digest().orders) << "same order flow";
    EXPECT_GE(opt.digest().filled_qty, cons.digest().filled_qty) << "seed " << seed;
    strictly += opt.digest().filled_qty > cons.digest().filled_qty ? 1 : 0;
  }
  EXPECT_GT(strictly, 0) << "the queue model must make a difference somewhere";
}

TEST(TradingConsistency, HashesArePinnedAcrossCompilers) {
  tk::GeneratorParams gp;
  gp.seed = 42;
  gp.messages = 5000;
  const DualResult r = run_trading(tk::generate_stream(gp), true);
  EXPECT_EQ(r.opt.orders, 0x9e2731535d5ae989ULL) << std::hex << "opt orders 0x" << r.opt.orders;
  EXPECT_EQ(r.opt.fills, 0x59ff6c4b1944ee7fULL) << std::hex << "opt fills 0x" << r.opt.fills;
  EXPECT_EQ(r.cons.fills, 0x405f2753e47ebceeULL) << std::hex << "cons fills 0x" << r.cons.fills;
  EXPECT_EQ(r.cons.position, 0xd58f4d45f97e13b6ULL) << std::hex << "cons position 0x" << r.cons.position;
}

TEST(TradingStage, StaleBookTripsTheKillSwitchAndCancelsOpenOrders) {
  // Valid session, then an order delete for an unknown ref -> feed Stale.
  tk::Buf s;
  tk::append_frame(s, tk::stock_directory(1, tk::symbol_of(1), 1));
  tk::append_frame(s, tk::add_order(1, 1, 'B', 500, tk::symbol_of(1), 999'000, 2));
  tk::append_frame(s, tk::add_order(1, 2, 'S', 500, tk::symbol_of(1), 1'001'000, 3));
  tk::append_frame(s, tk::order_delete(1, 999, 4));
  tk::append_frame(s, tk::add_order(1, 3, 'S', 500, tk::symbol_of(1), 1'000'900, 5));
  st::MarketMaker mm{};
  sim::TradingStage<st::MarketMaker> t{trading_cfg(sim::FillModel::Optimistic), mm};
  pl::BookStage<decltype(t)> stage{feed_cfg(), t};
  pl::MemorySource src{s};
  pl::run_single(src, stage, pl::RunConfig{});
  EXPECT_EQ(stage.feed().state(), md::FeedState::Stale);
  EXPECT_TRUE(t.risk().tripped());
  EXPECT_EQ(t.risk().trip_reason(), lob::risk::TripReason::StaleBook);
  EXPECT_EQ(t.exposure().open_buy + t.exposure().open_sell, 0U) << "every open order cancelled";
  EXPECT_EQ(t.exchange().engine().book().order_count(), 0U);
  EXPECT_EQ(t.digest().n_accepted, 2U) << "quotes before the stale point only";
}

TEST(TradingStage, ManualKillSwitchStillAllowsCancels) {
  tk::Buf s;
  tk::append_frame(s, tk::stock_directory(1, tk::symbol_of(1), 1));
  tk::append_frame(s, tk::add_order(1, 1, 'B', 500, tk::symbol_of(1), 999'000, 2));
  tk::append_frame(s, tk::add_order(1, 2, 'S', 500, tk::symbol_of(1), 1'001'000, 3));
  tk::append_frame(s, tk::add_order(1, 3, 'S', 500, tk::symbol_of(1), 1'000'500, 4));  // moves the mid
  st::MarketMaker mm{};
  sim::TradingStage<st::MarketMaker> t{trading_cfg(sim::FillModel::Optimistic), mm};
  // Trip before the third add: the requote's cancels go through, its news do not.
  struct Tripper {
    sim::TradingStage<st::MarketMaker>& t;
    void on_feed(const md::FeedInput& in, std::span<const md::FeedEvent> ev, const md::FeedHandler& f,
                 std::uint64_t ts, pl::LatencyRecorder* r, bool smp) {
      if (in.seq == 4) t.risk().trip(lob::risk::TripReason::Manual);
      t.on_feed(in, ev, f, ts, r, smp);
    }
  } tripper{t};
  pl::BookStage<Tripper> stage2{feed_cfg(), tripper};
  pl::MemorySource src{s};
  pl::run_single(src, stage2, pl::RunConfig{});
  EXPECT_TRUE(t.risk().tripped());
  EXPECT_EQ(t.digest().n_accepted, 2U);
  EXPECT_GE(t.risk().rejected(lob::risk::Verdict::KillSwitch), 1U);
  EXPECT_EQ(t.exposure().open_sell, 0U) << "the old ask was cancelled despite the kill switch";
}

}  // namespace
