// L4.5: verification of network order entry. docs/design/L4_DESIGN.md §4.
//
//   Lockstep  the strategy trades through OUCH over SoupBinTCP over loopback
//             TCP to the epoll exchange server, waiting for quiescence at
//             every step: orders / fills / position digests must equal the
//             in-process TradingStage's BIT FOR BIT (both fill models, file
//             replay and the multi-threaded pipeline).
//   Free-run  nothing waits: timing decides what the strategy sees. Checked
//             by invariants (no over-position, no duplicate order, legal
//             state machine, client / exchange reconciliation after a
//             drain) -- with injected disconnects and exchange throttling --
//             and by replay: the recorded run replays offline bit for bit
//             on both sides.
// Threads (server + client): label "concurrency", also run under TSan.

#include "lob/net/lockstep.hpp"

#include <gtest/gtest.h>

#include <thread>

#include "lob/net/epoll_server.hpp"
#include "lob/net/ouch_exchange.hpp"
#include "lob/pipeline/pipeline.hpp"
#include "lob/sim/trading.hpp"
#include "lob/testkit/itch_builder.hpp"
#include "lob/testkit/itch_generator.hpp"

namespace {

namespace md = lob::md;
namespace net = lob::net;
namespace pl = lob::pipeline;
namespace sim = lob::sim;
namespace st = lob::strategy;
namespace gw = lob::gateway;
namespace soup = lob::soup;
namespace tk = lob::testkit::itch;

#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
constexpr std::size_t kMessages = 2500;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
constexpr std::size_t kMessages = 2500;
#else
constexpr std::size_t kMessages = 8000;
#endif
#else
constexpr std::size_t kMessages = 8000;
#endif

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
  c.timer_interval_ns = 1'000'000;
  return c;
}

struct Result {
  std::uint64_t orders, fills, position;
  std::int64_t pos, pnl;
  std::uint64_t filled_qty, n_requests;
  bool operator==(const Result&) const = default;
};

template <class T>
Result result_of(const T& t) {
  return Result{t.digest().orders, t.digest().fills, t.position_hash(), t.position(), t.pnl(), t.digest().filled_qty,
                t.digest().n_requests};
}

template <class Stage>
void run(const tk::Buf& stream, Stage& stage, bool pipelined) {
  pl::MemorySource src{stream};
  pl::RunConfig rc;
  rc.chunk_bytes = 1U << 17;
  rc.queue_capacity = 256;
  if (pipelined) pl::run_pipelined(src, stage, rc); else pl::run_single(src, stage, rc);
}

Result in_process(const tk::Buf& stream, sim::FillModel m, bool pipelined) {
  st::MarketMaker mm{};
  sim::TradingStage<st::MarketMaker> t{trading_cfg(m), mm};
  pl::BookStage<decltype(t)> stage{feed_cfg(), t};
  run(stream, stage, pipelined);
  return result_of(t);
}

net::ExchangeConfig exchange_cfg(sim::FillModel m, std::uint32_t throttle) {
  net::ExchangeConfig c;
  c.feed = feed_cfg();
  c.sim.model = m;
  c.accounts = {net::AccountConfig{"A1", "pw", "ZKQT", true, throttle, throttle == 0 ? 1U : 20U}};
  c.log_messages = 1U << 18;
  c.log_bytes = 1U << 24;
  c.max_orders = 1U << 17;
  c.track_exposure = true;
  return c;
}

gw::GatewayConfig gateway_cfg() {
  gw::GatewayConfig g;
  g.symbol = feed_cfg().symbol;
  g.day = "1105";
  g.instance = 0;
  g.max_orders = 1U << 17;
  return g;
}

constexpr std::uint64_t kHeartbeat = 20'000'000;  // 20 ms in tests: reconnects resolve quickly

/// Exchange + epoll server on its own thread, journaled.
struct Rig {
  net::ExchangeConfig ec;
  net::OuchExchange ex;
  net::EpollServer srv;
  net::ServerJournal journal;
  std::thread th;

  static net::ServerOptions options(bool auto_events) {
    net::ServerOptions o;
    o.auto_market_events = auto_events;
    o.timers.heartbeat_ns = kHeartbeat;
    o.control_capacity = 1U << 14;
    return o;
  }
  Rig(sim::FillModel m, bool auto_events, std::uint32_t throttle = 0)
      : ec(exchange_cfg(m, throttle)), ex(ec), srv(ex, options(auto_events)) {
    journal.entries.reserve(1U << 16);
    srv.set_journal(&journal);
    th = std::thread([this] { srv.run(); });
  }
  ~Rig() { stop(); }
  void stop() {
    if (!th.joinable()) return;
    net::Control c;
    c.kind = net::Control::Kind::Stop;
    while (!srv.post(c)) std::this_thread::yield();
    th.join();
  }
};

soup::ClientConfig client_cfg() {
  soup::ClientConfig c;
  c.user = "A1";
  c.password = "pw";
  c.timers.heartbeat_ns = kHeartbeat;
  return c;
}

bool login(net::ClientConn& conn, std::uint16_t port) {
  if (!conn.connect(port)) return false;
  for (int i = 0; i < 5000 && conn.session().state() == soup::ClientState::LoginSent; ++i) {
    (void)conn.poll(1, [](std::uint64_t, lob::ouch::Bytes) {});
  }
  return conn.session().state() == soup::ClientState::LoggedIn;
}

// --- Lockstep ---------------------------------------------------------------------------------

struct NetResult {
  Result r;
  net::NetStats stats;
  bool ok;
};

NetResult lockstep(const tk::Buf& stream, sim::FillModel m, bool pipelined) {
  Rig rig(m, /*auto_events=*/false);
  net::ClientConn conn(client_cfg());
  EXPECT_TRUE(login(conn, rig.srv.port()));
  gw::OrderGateway g(gateway_cfg());
  st::MarketMaker mm{};
  sim::TradingStage<st::MarketMaker, net::LockstepExchange> t{trading_cfg(m), mm,
                                                              net::LockstepExchange(rig.srv, conn, g, 0)};
  net::NetStage<decltype(t)> ns{t};
  pl::BookStage<decltype(ns)> stage{feed_cfg(), ns};
  // Start of Day arrives before anything else; deliver it so that the
  // client's sequence counter matches before the first barrier.
  (void)conn.poll(50, [&](std::uint64_t, lob::ouch::Bytes p) {
    sim::Reports r;
    g.on_message(p, r);
  });
  run(stream, stage, pipelined);
  rig.stop();
  const bool ok = !t.exchange().failed() && !g.inconsistent() && !t.risk().tripped();
  return NetResult{result_of(t), t.exchange().stats(), ok};
}

TEST(Lockstep, NetworkOrderEntryMatchesInProcessBitForBit) {
  for (std::uint64_t seed = 1; seed <= 2; ++seed) {
    tk::GeneratorParams gp;
    gp.seed = seed;
    gp.messages = kMessages;
    const tk::Buf s = tk::generate_stream(gp);
    for (const auto m : {sim::FillModel::Optimistic, sim::FillModel::Conservative}) {
      const Result ref = in_process(s, m, false);
      const NetResult net = lockstep(s, m, false);
      ASSERT_TRUE(net.ok) << "seed " << seed;
      EXPECT_EQ(net.r, ref) << "seed " << seed << " model " << static_cast<int>(m);
      EXPECT_GT(ref.n_requests, 50U) << "the strategy must actually trade";
      EXPECT_GT(ref.filled_qty, 0U);
      EXPECT_GT(net.stats.events_sent, 0U);
    }
  }
}

TEST(Lockstep, MultiThreadedPipelineAlsoMatches) {
  tk::GeneratorParams gp;
  gp.seed = 3;
  gp.messages = kMessages;
  const tk::Buf s = tk::generate_stream(gp);
  const Result ref = in_process(s, sim::FillModel::Conservative, false);
  const NetResult net = lockstep(s, sim::FillModel::Conservative, /*pipelined=*/true);
  ASSERT_TRUE(net.ok);
  EXPECT_EQ(net.r, ref);
}

// --- Free run ----------------------------------------------------------------------------------

struct FreeRun {
  Result r{};
  bool ok{false};
};

/// Runs free, checks the invariants and the reconciliation, then replays
/// both journals offline and compares.
FreeRun free_run(const tk::Buf& stream, sim::FillModel m, std::vector<std::uint64_t> drops, std::uint32_t throttle) {
  const auto tc = trading_cfg(m);
  Rig rig(m, /*auto_events=*/true, throttle);
  net::ClientConn conn(client_cfg());
  EXPECT_TRUE(login(conn, rig.srv.port()));
  gw::OrderGateway g(gateway_cfg());
  net::ClientJournal cj;
  cj.ops.reserve(1U << 16);
  st::MarketMaker mm{};
  sim::TradingStage<st::MarketMaker, net::FreeRunExchange> t{tc, mm, net::FreeRunExchange(rig.srv, conn, g, 0, &cj)};
  t.exchange().drop_at(drops);
  net::NetStage<decltype(t)> ns{t};
  pl::BookStage<decltype(ns)> stage{feed_cfg(), ns};
  run(stream, stage, false);
  sim::Reports rest;
  t.exchange().drain(rest);
  rig.stop();
  const auto& fx = t.exchange();
  FreeRun out;
  out.r = result_of(t);

  // Invariants.
  EXPECT_FALSE(fx.failed()) << fx.failure() << " state " << lob::soup::to_string(conn.session().state())
                            << " close " << lob::soup::to_string(conn.session().close_reason());
  EXPECT_FALSE(g.inconsistent()) << "illegal transitions " << g.counters().illegal;
  EXPECT_EQ(fx.stats().reconnects, drops.size());
  EXPECT_LE(fx.peak_exposure(), tc.risk.max_position) << "client view over the position limit";
  const auto p = rig.ex.position(0);
  EXPECT_LE(p.peak_exposure, tc.risk.max_position) << "exchange view over the position limit";
  EXPECT_EQ(rig.ex.counters().duplicate_tokens, 0U) << "an order was entered twice";
  // Reconciliation after the drain.
  EXPECT_EQ(fx.position(), p.position);
  EXPECT_EQ(fx.cash(), p.cash);
  EXPECT_EQ(p.live_orders, 0U);
  std::array<std::uint64_t, 4> any{};
  EXPECT_EQ(g.open_ids(any), 0U);
  EXPECT_EQ(conn.session().next_sequence(), rig.ex.log(0).next_sequence()) << "sequence gap";

  // Replay, exchange side: the journal into a fresh exchange.
  {
    net::OuchExchange again(rig.ec);
    net::replay(rig.journal, again, /*auto_market_events=*/true);
    EXPECT_EQ(again.outbound_hash(0), rig.ex.outbound_hash(0)) << "exchange replay diverged";
    EXPECT_EQ(again.position(0).position, p.position);
  }
  // Replay, client side: the recorded delivery points, the same messages.
  {
    gw::OrderGateway g2(gateway_cfg());
    st::MarketMaker mm2{};
    sim::TradingStage<st::MarketMaker, net::ReplayExchange> t2{tc, mm2, net::ReplayExchange(g2, rig.ex.log(0), cj)};
    net::NetStage<decltype(t2)> ns2{t2};
    pl::BookStage<decltype(ns2)> stage2{feed_cfg(), ns2};
    run(stream, stage2, false);
    EXPECT_TRUE(t2.exchange().complete()) << "client replay diverged";
    EXPECT_EQ(result_of(t2), out.r) << "client replay digests differ";
  }
  out.ok = !::testing::Test::HasFailure();
  return out;
}

TEST(FreeRun, InvariantsHoldAndTheRecordedRunReplaysBitForBit) {
  for (std::uint64_t seed = 1; seed <= 2; ++seed) {
    tk::GeneratorParams gp;
    gp.seed = seed;
    gp.messages = kMessages;
    const tk::Buf s = tk::generate_stream(gp);
    for (const auto m : {sim::FillModel::Optimistic, sim::FillModel::Conservative}) {
      const FreeRun f = free_run(s, m, {}, 0);
      ASSERT_TRUE(f.ok) << "seed " << seed;
      EXPECT_GT(f.r.n_requests, 50U);
    }
  }
}

tk::Buf fault_stream() {
  tk::GeneratorParams gp;
  gp.seed = 4;
  gp.messages = kMessages;
  return tk::generate_stream(gp);
}

// Drop points scale with the stream: the target instrument has roughly one
// book update per 5-10 messages.
TEST(FreeRun, DisconnectsKeepTheInvariants) {
  // Three connection drops; cancel-on-disconnect is on for the account.
  EXPECT_TRUE(free_run(fault_stream(), sim::FillModel::Optimistic, {kMessages / 400, kMessages / 50, kMessages / 20}, 0).ok);
}

TEST(FreeRun, ExchangeThrottlingAndADisconnectKeepTheInvariants) {
  // Exchange-side throttle well below the strategy's request rate: Rejected 'x'.
  EXPECT_TRUE(free_run(fault_stream(), sim::FillModel::Conservative, {kMessages / 50}, 2000).ok);
}

}  // namespace
