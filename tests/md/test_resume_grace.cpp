// Resume grace (D-055). Real data (Nasdaq 2019-01-30, SXTC, two LULD pauses)
// showed: Cross Trade 'H' and Trading Action 'T' arrive together, and the
// executions of the crossing orders follow AFTER the 'T' (33 and 37 frames,
// 233 and 264 us later). Without a grace the feed went Stale for the rest of
// the day. One named test per rule; the first sample reproduces that order.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lob/md/feed.hpp"
#include "lob/md/stream.hpp"
#include "lob/testkit/itch_builder.hpp"
#include "lob/testkit/itch_samples.hpp"

namespace {

namespace md = lob::md;
namespace tk = lob::testkit::itch;
using md::AlertReason;
using md::FeedEvent;
using md::FeedState;
constexpr std::uint16_t kLoc = lob::testkit::itch_samples::kLocate;
constexpr std::uint64_t kSec = 1'000'000'000ULL;

struct Script {
  explicit Script(const md::FeedConfig& c = lob::testkit::itch_samples::config()) : h{c} {}
  md::FeedHandler h;
  std::vector<FeedEvent> events;
  std::string text;
  std::uint64_t seq{1};

  Script& feed(const tk::Buf& msg) {
    tk::Buf stream;
    tk::append_frame(stream, msg);
    md::decode_frames(stream, seq, [&](const md::FeedInput& in) {
      std::vector<FeedEvent> ev;
      h.on_input(in, ev);
      for (const auto& e : ev) {
        events.push_back(e);
        md::append_text(e, text);
      }
    });
    ++seq;
    return *this;
  }
  [[nodiscard]] const FeedEvent& last() const { return events.back(); }
};

tk::Buf add(std::uint64_t ref, char side, std::uint32_t qty, std::uint32_t px, std::uint64_t ts) {
  return tk::add_order(kLoc, ref, side, qty, "ACME", px, ts);
}
tk::Buf state(char st, std::uint64_t ts, std::string_view reason = "") {
  return tk::trading_action(kLoc, "ACME", st, ts, reason);
}
tk::Buf cross(std::uint64_t shares, std::uint32_t px, char type, std::uint64_t ts) {
  return tk::cross_trade(kLoc, "ACME", shares, px, type, ts);
}
tk::Buf exec_n(std::uint64_t ref, std::uint32_t qty, std::uint32_t px, std::uint64_t ts) {
  return tk::order_executed_price(kLoc, ref, qty, 0, 'N', px, ts);  // cross execution: printable 'N'
}

/// seq 1-7: regular trading, bid 5.81 x 100, ask 5.86 x 100 (strict);
/// seq 8: LULD pause; seq 9-10: the book crosses during the pause.
void paused_and_crossed(Script& r) {
  r.feed(tk::system_event('O', 1)).feed(tk::stock_directory(kLoc, "ACME", 2)).feed(tk::system_event('S', 3))
      .feed(state('T', 4)).feed(tk::system_event('Q', 5)).feed(add(1, 'B', 100, 58100, 6))
      .feed(add(2, 'S', 100, 58600, 7));
  ASSERT_TRUE(r.h.strict());
  r.feed(state('P', 8, "LUDP")).feed(add(3, 'B', 400, 64000, 9)).feed(add(4, 'S', 300, 57500, 10));
  ASSERT_EQ(r.last().cross, 'X');
}

// The real-data order of events (SXTC 10:32:49.516895423): Q 'H', H 'T' at
// the same timestamp, then the cross executions (C, printable N).
TEST(ResumeGrace, HaltCrossExecutionsAfterTheResumeAreTolerated) {
  Script r;
  paused_and_crossed(r);
  r.feed(cross(400, 58600, 'H', 20));   // seq 11: reopening cross, 400 @ 5.86
  r.feed(state('T', 20));               // seq 12: trading resumes, book still crossed -> grace
  r.feed(exec_n(4, 300, 58600, 21));    // seq 13: still crossed (bid 6.40 > ask 5.86)
  r.feed(exec_n(2, 100, 58600, 22));    // seq 14: ask side empty -> clean -> resolved
  r.feed(exec_n(3, 400, 58600, 23));    // seq 15: the last cross execution, now strict
  r.feed(add(5, 'S', 10, 58000, 24));   // seq 16: crossing add in strict mode -> Stale
  const std::string expected =
      "seq=1 ts=1 SYSTEM state=UNINITIALIZED reason=- detail=79 type=S ref=0 bid=0x0 ask=0x0 sess=O trd=- cross=-\n"
      "seq=2 ts=2 STATE state=LIVE reason=- detail=0 type=R ref=0 bid=0x0 ask=0x0 sess=O trd=- cross=-\n"
      "seq=3 ts=3 SYSTEM state=LIVE reason=- detail=83 type=S ref=0 bid=0x0 ask=0x0 sess=S trd=- cross=-\n"
      "seq=4 ts=4 TRADING state=LIVE reason=- detail=84 type=H ref=0 bid=0x0 ask=0x0 sess=S trd=T cross=-\n"
      "seq=5 ts=5 SYSTEM state=LIVE reason=- detail=81 type=S ref=0 bid=0x0 ask=0x0 sess=Q trd=T cross=-\n"
      "seq=6 ts=6 BOOK state=LIVE reason=- detail=0 type=A ref=1 bid=58100x100 ask=0x0 msg=B58100x100 sess=Q trd=T cross=-\n"
      "seq=7 ts=7 BOOK state=LIVE reason=- detail=0 type=A ref=2 bid=58100x100 ask=58600x100 msg=S58600x100 sess=Q trd=T cross=-\n"
      "seq=8 ts=8 TRADING state=LIVE reason=- detail=80 type=H ref=0 bid=58100x100 ask=58600x100 sess=Q trd=P cross=-\n"
      "seq=9 ts=9 BOOK state=LIVE reason=- detail=0 type=A ref=3 bid=64000x400 ask=58600x100 msg=B64000x400 sess=Q trd=P cross=X\n"
      "seq=10 ts=10 BOOK state=LIVE reason=- detail=0 type=A ref=4 bid=64000x400 ask=57500x300 msg=S57500x300 sess=Q trd=P cross=X\n"
      "seq=11 ts=20 CROSS state=LIVE reason=- detail=72 type=Q ref=0 bid=64000x400 ask=57500x300 sess=Q trd=P cross=X\n"
      "seq=12 ts=20 TRADING state=LIVE reason=- detail=84 type=H ref=0 bid=64000x400 ask=57500x300 sess=Q trd=T cross=X\n"
      "seq=12 ts=20 ALERT state=LIVE reason=RESUME_GRACE detail=88 type=H ref=0 bid=64000x400 ask=57500x300 sess=Q trd=T cross=X grace=G\n"
      "seq=13 ts=21 BOOK state=LIVE reason=- detail=0 type=C ref=4 bid=64000x400 ask=58600x100 msg=S57500x300 sess=Q trd=T cross=X grace=G\n"
      "seq=14 ts=22 BOOK state=LIVE reason=- detail=0 type=C ref=2 bid=64000x400 ask=0x0 msg=S58600x100 sess=Q trd=T cross=- grace=G\n"
      "seq=14 ts=22 ALERT state=LIVE reason=GRACE_RESOLVED detail=0 type=C ref=0 bid=64000x400 ask=0x0 sess=Q trd=T cross=-\n"
      "seq=15 ts=23 BOOK state=LIVE reason=- detail=0 type=C ref=3 bid=58100x100 ask=0x0 msg=B64000x400 sess=Q trd=T cross=-\n"
      "seq=16 ts=24 STATE state=STALE reason=BOOK_REJECTED detail=7 type=A ref=0 bid=58100x100 ask=0x0 sess=Q trd=T cross=-\n";
  EXPECT_EQ(r.text, expected);
  EXPECT_EQ(r.h.counters().graces, 1U);
  EXPECT_EQ(r.h.counters().graces_resolved, 1U);
  EXPECT_EQ(r.h.counters().cross_trades, 1U);
}

TEST(ResumeGrace, ACrossTradeArrivingAfterTheResumeAlsoCounts) {
  Script r;
  paused_and_crossed(r);
  r.feed(state('T', 20));
  ASSERT_TRUE(r.h.in_grace());
  r.feed(exec_n(4, 300, 58600, 21)).feed(exec_n(2, 100, 58600, 22));
  EXPECT_TRUE(r.h.in_grace()) << "book clean, but no cross trade seen yet";
  EXPECT_FALSE(md::tradable(r.last()));
  r.feed(cross(400, 58600, 'H', 23));
  EXPECT_FALSE(r.h.in_grace());
  EXPECT_TRUE(r.h.strict());
  EXPECT_EQ(r.last().reason, AlertReason::GraceResolved);
  EXPECT_EQ(r.h.state(), FeedState::Live);
}

TEST(ResumeGrace, ExpiresAfterTheMarketTimeLimit) {
  Script r;
  paused_and_crossed(r);
  r.feed(cross(400, 58600, 'H', 20)).feed(state('T', 20));
  ASSERT_TRUE(r.h.in_grace());
  r.feed(tk::order_delete(9, 77, 20 + kSec));  // another instrument, exactly at the limit: still in grace
  EXPECT_TRUE(r.h.in_grace());
  r.feed(tk::order_delete(9, 78, 20 + kSec + 1));  // one ns past the limit
  EXPECT_EQ(r.h.state(), FeedState::Stale);
  EXPECT_EQ(r.last().reason, AlertReason::CrossedAtResume);
  EXPECT_EQ(r.last().detail, 'X');
}

TEST(ResumeGrace, ExpiresAfterTheMessageLimit) {
  md::FeedConfig c = lob::testkit::itch_samples::config();
  c.resume_grace_messages = 3;
  Script r{c};
  paused_and_crossed(r);
  r.feed(cross(400, 58600, 'H', 20)).feed(state('T', 20));
  for (std::uint64_t i = 0; i < 3; ++i) r.feed(add(10 + i, 'B', 1, 50000, 21));  // nothing uncrosses
  EXPECT_TRUE(r.h.in_grace());
  r.feed(add(20, 'B', 1, 50000, 21));
  EXPECT_EQ(r.h.state(), FeedState::Stale);
  EXPECT_EQ(r.last().reason, AlertReason::CrossedAtResume);
}

TEST(ResumeGrace, ACleanBookWithoutACrossTradeIsNotEnough) {
  Script r;
  paused_and_crossed(r);
  r.feed(state('T', 20));
  r.feed(tk::order_delete(kLoc, 3, 21)).feed(tk::order_delete(kLoc, 4, 21));  // cleared by deletes, no cross
  EXPECT_EQ(r.last().cross, '-');
  EXPECT_TRUE(r.h.in_grace());
  r.feed(tk::order_delete(9, 1, 21 + 2 * kSec));
  EXPECT_EQ(r.h.state(), FeedState::Stale);
  EXPECT_EQ(r.last().detail, '-') << "expired on a clean book: the cross never confirmed the resumption";
}

TEST(ResumeGrace, NothingIsTradableDuringTheGrace) {
  Script r;
  paused_and_crossed(r);
  r.feed(cross(400, 58600, 'H', 20)).feed(state('T', 20)).feed(exec_n(4, 300, 58600, 21));
  for (const auto& e : r.events) {
    if (e.grace == 'G') {
      EXPECT_FALSE(md::tradable(e));
    }
  }
  r.feed(exec_n(2, 100, 58600, 22));
  EXPECT_TRUE(md::tradable(r.last())) << "the GRACE_RESOLVED event";
}

TEST(ResumeGrace, ANewPauseCancelsTheGrace) {
  Script r;
  paused_and_crossed(r);
  r.feed(cross(400, 58600, 'H', 20)).feed(state('T', 20));
  ASSERT_TRUE(r.h.in_grace());
  r.feed(state('P', 21, "LUDP"));
  EXPECT_FALSE(r.h.in_grace());
  EXPECT_FALSE(r.h.strict());
  r.feed(tk::order_delete(9, 1, 21 + 5 * kSec));
  EXPECT_EQ(r.h.state(), FeedState::Live) << "no grace is running, so nothing expires";
  // The next resumption needs its own cross trade.
  r.feed(state('T', 22 + 5 * kSec));
  ASSERT_TRUE(r.h.in_grace());
  r.feed(exec_n(4, 300, 58600, 23 + 5 * kSec)).feed(exec_n(2, 100, 58600, 23 + 5 * kSec));
  EXPECT_TRUE(r.h.in_grace()) << "the earlier cross belonged to the cancelled resumption";
}

TEST(ResumeGrace, DisabledMeansImmediateStale) {
  md::FeedConfig c = lob::testkit::itch_samples::config();
  c.resume_grace_messages = 0;
  Script r{c};
  paused_and_crossed(r);
  r.feed(cross(400, 58600, 'H', 20)).feed(state('T', 20));
  EXPECT_EQ(r.h.state(), FeedState::Stale);
  EXPECT_EQ(r.last().reason, AlertReason::CrossedAtResume);
  EXPECT_EQ(r.h.counters().graces, 0U);
}

TEST(ResumeGrace, OpeningCrossOnACrossedBook) {
  Script r;
  r.feed(tk::system_event('S', 1)).feed(tk::stock_directory(kLoc, "ACME", 2)).feed(state('T', 3));
  r.feed(add(1, 'B', 200, 10100, 4)).feed(add(2, 'S', 200, 10000, 5));  // crossed pre-market
  r.feed(tk::system_event('Q', 6));
  EXPECT_TRUE(r.h.in_grace());
  EXPECT_EQ(r.h.state(), FeedState::Live);
  r.feed(cross(200, 10050, 'O', 7)).feed(exec_n(1, 200, 10050, 8));
  EXPECT_FALSE(r.h.in_grace());
  EXPECT_TRUE(r.h.strict());
  EXPECT_EQ(r.h.counters().graces_resolved, 1U);
}

TEST(ResumeGrace, CrossTradesOfOtherInstrumentsDoNotCount) {
  Script r;
  paused_and_crossed(r);
  r.feed(tk::cross_trade(9, "ZZZZ", 400, 58600, 'H', 20)).feed(state('T', 20));
  r.feed(exec_n(4, 300, 58600, 21)).feed(exec_n(2, 100, 58600, 22));
  EXPECT_TRUE(r.h.in_grace());
  EXPECT_EQ(r.h.counters().cross_trades, 0U);
}

}  // namespace
