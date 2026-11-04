// Market-wide mode (D-056): every instrument's events and book equal those
// of a standalone single-instrument FeedHandler on the same stream; one
// instrument going Stale does not affect the others; single-threaded and
// sharded runs (N = 1..4, any queue size) give identical per-instrument
// digests and summary hash. Label "concurrency" (TSan presets).

#include "lob/pipeline/market_pipeline.hpp"

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "lob/testkit/itch_builder.hpp"
#include "lob/testkit/itch_generator.hpp"

namespace {

namespace md = lob::md;
namespace pl = lob::pipeline;
namespace tk = lob::testkit::itch;

md::MarketConfig mcfg() {
  md::MarketConfig c;
  c.max_orders_per_shard = 1U << 14;
  c.max_levels_per_side = 1024;
  c.initial_levels_per_side = 4;
  return c;
}

pl::MarketRun run(const tk::Buf& s, std::size_t shards, std::size_t queue = 64, std::size_t max_read = SIZE_MAX) {
  pl::MemorySource src{s, max_read};
  pl::RunConfig rc;
  rc.chunk_bytes = 1U << 17;
  rc.queue_capacity = queue;
  return pl::run_market(src, mcfg(), shards, rc);
}

/// The reference: a standalone single-instrument handler on the whole stream.
std::pair<std::uint64_t, std::uint64_t> standalone(const tk::Buf& s, const std::string& sym) {
  md::FeedConfig c;
  c.symbol = md::make_symbol(sym);
  c.book = md::BookConfig{1U << 14, 1024};
  md::FeedHandler h{c};
  std::uint64_t hash = md::kHashSeed;
  std::vector<md::FeedEvent> ev;
  const auto p = md::decode_frames(s, 1, [&](const md::FeedInput& in) {
    ev.clear();
    h.on_input(in, ev);
    for (const auto& e : ev) hash = md::hash_event(e, hash);
  });
  if (p.last != md::itch::FrameStatus::End) {
    md::FeedInput fe{};
    fe.seq = p.next_seq;
    fe.kind = md::InputKind::FrameError;
    ev.clear();
    h.on_input(fe, ev);
    for (const auto& e : ev) hash = md::hash_event(e, hash);
  }
  return {hash, h.book().state_hash()};
}

const md::StockResult& find(const pl::MarketRun& r, const std::string& sym) {
  for (const auto& s : r.summary.stocks) {
    if (s.symbol == sym) return s;
  }
  ADD_FAILURE() << "no " << sym;
  return r.summary.stocks.front();
}

/// Two instruments interleaved message by message, plus a third with no orders.
tk::Buf interleaved(bool break_acme) {
  tk::Buf s;
  auto put = [&](const tk::Buf& m) { tk::append_frame(s, m); };
  put(tk::system_event('O', 1));
  put(tk::stock_directory(3, "ACME", 2));
  put(tk::stock_directory(8, "ZZZZ", 3));
  put(tk::stock_directory(5, "IDLE", 3));
  put(tk::system_event('S', 4));
  put(tk::trading_action(3, "ACME", 'T', 5));
  put(tk::trading_action(8, "ZZZZ", 'T', 5));
  put(tk::system_event('Q', 6));
  put(tk::add_order(3, 1, 'B', 100, "ACME", 10000, 7));
  put(tk::add_order(8, 2, 'B', 300, "ZZZZ", 50000, 7));
  put(tk::add_order(3, 3, 'S', 100, "ACME", 10100, 8));
  put(tk::add_order(8, 4, 'S', 200, "ZZZZ", 50100, 8));
  put(tk::order_executed(8, 2, 100, 1, 9));
  put(break_acme ? tk::order_delete(3, 999, 9) : tk::order_executed(3, 1, 50, 2, 9));
  put(tk::trading_action(8, "ZZZZ", 'H', 10));
  put(tk::add_order(8, 5, 'S', 50, "ZZZZ", 49000, 11));  // crosses during ZZZZ's halt
  put(tk::order_replace(3, 3, 6, 100, 10050, 12));
  put(tk::order_delete(8, 5, 13));
  put(tk::trading_action(8, "ZZZZ", 'T', 14));
  put(tk::order_cancel(8, 4, 50, 15));
  put(tk::system_event('M', 16));
  put(tk::system_event('C', 17));
  return s;
}

TEST(Market, EachInstrumentEqualsAStandaloneSingleInstrumentHandler) {
  const tk::Buf s = interleaved(false);
  const auto r = run(s, 0);
  ASSERT_EQ(r.summary.stocks.size(), 3U);
  for (const char* sym : {"ACME", "ZZZZ", "IDLE"}) {
    const auto [eh, bh] = standalone(s, sym);
    EXPECT_EQ(find(r, sym).event_hash, eh) << sym;
    EXPECT_EQ(find(r, sym).book_hash, bh) << sym;
    EXPECT_EQ(find(r, sym).state, md::FeedState::Live) << sym;
  }
  EXPECT_EQ(find(r, "ZZZZ").counters.crossed_updates, 1U) << "crossed during its own halt only";
  EXPECT_EQ(find(r, "ACME").counters.crossed_updates, 0U);
  EXPECT_EQ(r.summary.live, 3U);
}

TEST(Market, OneInstrumentGoingStaleDoesNotAffectTheOthers) {
  const tk::Buf s = interleaved(true);
  const auto r = run(s, 0);
  EXPECT_EQ(find(r, "ACME").state, md::FeedState::Stale);
  EXPECT_EQ(find(r, "ACME").stale_reason, md::AlertReason::BookRejected);
  EXPECT_EQ(find(r, "ZZZZ").state, md::FeedState::Live);
  EXPECT_EQ(find(r, "IDLE").state, md::FeedState::Live);
  for (const char* sym : {"ACME", "ZZZZ"}) {
    EXPECT_EQ(find(r, sym).event_hash, standalone(s, sym).first) << sym;
  }
  EXPECT_EQ(r.summary.stale, 1U);
  EXPECT_EQ(r.summary.stale_reasons.at("BOOK_REJECTED"), 1U);
}

TEST(Market, AStreamFaultMakesEveryInstrumentStale) {
  tk::Buf s = interleaved(false);
  s.resize(s.size() - 5);  // truncated tail -> FrameError
  const auto r = run(s, 0);
  EXPECT_EQ(r.summary.stale, 3U);
  EXPECT_EQ(r.summary.stale_reasons.at("FRAME_ERROR"), 3U);
  for (const char* sym : {"ACME", "ZZZZ"}) {
    EXPECT_EQ(find(r, sym).event_hash, standalone(s, sym).first) << sym;
  }
}

TEST(Market, RouterTurnsASequenceGapIntoABroadcastFault) {
  md::Router router{3};
  std::vector<std::pair<std::size_t, md::Routed>> got;
  auto emit = [&](std::size_t k, const md::Routed& r) { got.push_back({k, r}); };
  md::FeedInput a{};
  a.seq = 1;
  a.msg = md::itch::OrderDelete{md::itch::Header{'D', 4, 0, 10}, 1};
  router.on_input(a, emit);
  ASSERT_EQ(got.size(), 1U);
  EXPECT_EQ(got[0].first, 1U) << "locate 4 % 3";
  got.clear();
  md::FeedInput b = a;
  b.seq = 5;  // 2..4 missing
  router.on_input(b, emit);
  ASSERT_EQ(got.size(), 4U) << "a fault to each shard, then the message";
  for (std::size_t k = 0; k < 3; ++k) {
    EXPECT_EQ(got[k].second.kind, md::Routed::Kind::Fault);
    EXPECT_EQ(got[k].second.fault, md::AlertReason::SequenceGap);
  }
  EXPECT_EQ(got[3].second.kind, md::Routed::Kind::Stock);
}

TEST(Market, MessagesBeforeTheDirectoryAreCountedAsUnknownLocate) {
  tk::Buf s;
  tk::append_frame(s, tk::add_order(3, 1, 'B', 1, "ACME", 100, 1));
  tk::append_frame(s, tk::stock_directory(3, "ACME", 2));
  const auto r = run(s, 0);
  EXPECT_EQ(r.summary.unknown_locate, 1U);
  EXPECT_EQ(find(r, "ACME").orders, 0U);
}

#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
constexpr std::size_t kMessages = 6000;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
constexpr std::size_t kMessages = 6000;
#else
constexpr std::size_t kMessages = 60000;
#endif
#else
constexpr std::size_t kMessages = 60000;
#endif

void expect_same(const pl::MarketRun& a, const pl::MarketRun& b, const std::string& what) {
  ASSERT_EQ(a.summary.stocks.size(), b.summary.stocks.size()) << what;
  for (std::size_t i = 0; i < a.summary.stocks.size(); ++i) {
    const auto& x = a.summary.stocks[i];
    const auto& y = b.summary.stocks[i];
    ASSERT_EQ(x.locate, y.locate) << what;
    EXPECT_EQ(x.event_hash, y.event_hash) << what << " " << x.symbol;
    EXPECT_EQ(x.book_hash, y.book_hash) << what << " " << x.symbol;
    EXPECT_EQ(x.state, y.state) << what << " " << x.symbol;
    EXPECT_EQ(x.events, y.events) << what << " " << x.symbol;
  }
  EXPECT_EQ(a.summary.summary_hash, b.summary.summary_hash) << what;
}

TEST(Market, ShardedEqualsSingleThreadedForEveryInstrument) {
  for (std::uint64_t seed = 1; seed <= 2; ++seed) {
    tk::GeneratorParams gp;
    gp.seed = seed;
    gp.messages = kMessages;
    gp.instruments = 13;
    gp.max_live_per_instrument = 150;
    const tk::Buf s = tk::generate_stream(gp);
    const auto ref = run(s, 0);
    ASSERT_EQ(ref.summary.stocks.size(), 13U);
    ASSERT_EQ(ref.summary.live, 13U);
    for (const std::size_t n : {1U, 2U, 3U, 4U}) {
      for (const std::size_t queue : {1U, 256U}) {
        expect_same(ref, run(s, n, queue, 4093),
                    "seed " + std::to_string(seed) + " N " + std::to_string(n) + " queue " + std::to_string(queue));
      }
    }
    // And each instrument equals its standalone handler.
    for (const char* sym : {"SYM1", "SYM7", "SYM13"}) {
      EXPECT_EQ(find(ref, sym).event_hash, standalone(s, sym).first) << sym;
      EXPECT_EQ(find(ref, sym).book_hash, standalone(s, sym).second) << sym;
    }
  }
}

TEST(Market, PoolPeaksAreBoundedByTheirShardsAndExactWithOneShard) {
  tk::GeneratorParams gp;
  gp.messages = kMessages;
  gp.instruments = 9;
  const tk::Buf s = tk::generate_stream(gp);
  const auto one = run(s, 0);
  const auto four = run(s, 4);
  ASSERT_EQ(one.shard_peaks.size(), 1U);
  EXPECT_GT(one.shard_peaks[0], 0U);
  std::size_t sum = 0;
  for (const auto p : four.shard_peaks) {
    EXPECT_LE(p, one.shard_peaks[0]);
    sum += p;
  }
  EXPECT_GE(sum, one.shard_peaks[0]) << "the shards' peaks bound the market peak from above";
  EXPECT_EQ(four.shard_instruments.size(), 4U);
}

}  // namespace
