// Trading-phase awareness (D-054): the "never locked / never crossed" rule
// is enforced only in regular hours (System Event 'Q' .. 'M') while the
// instrument is trading (Trading Action 'T'). One named test per
// transition, plus the complete "crossed during a halt -> uncrossed ->
// trading resumes" sequence with its expected event stream.

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

struct Script {
  md::FeedHandler h{lob::testkit::itch_samples::config()};
  std::vector<FeedEvent> events;
  std::string text;
  std::uint64_t seq{1};
  std::uint64_t ts{1};

  /// Feeds one message (fresh seq and a non-decreasing timestamp).
  Script& operator<<(const tk::Buf& msg) {
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
  [[nodiscard]] std::uint64_t t() { return ts++; }
  [[nodiscard]] const FeedEvent& last() const { return events.back(); }
};

tk::Buf add(std::uint64_t ref, char side, std::uint32_t qty, std::uint32_t px, std::uint64_t ts) {
  return tk::add_order(kLoc, ref, side, qty, "ACME", px, ts);
}
tk::Buf halt(char state, std::uint64_t ts) { return tk::trading_action(kLoc, "ACME", state, ts); }

/// Directory, start of system hours, ACME trading, market open, a clean book.
void open_market(Script& r) {
  r << tk::system_event('O', r.t()) << tk::stock_directory(kLoc, "ACME", r.t()) << tk::system_event('S', r.t())
    << halt('T', r.t()) << tk::system_event('Q', r.t()) << add(1, 'B', 100, 10000, r.t())
    << add(2, 'S', 100, 10100, r.t());
  ASSERT_TRUE(r.h.strict());
  ASSERT_EQ(r.h.state(), FeedState::Live);
}

TEST(TradingPhase, NothingKnownYetIsNotStrict) {
  Script r;
  r << tk::stock_directory(kLoc, "ACME", r.t()) << add(1, 'B', 100, 10000, r.t()) << add(2, 'S', 10, 9900, r.t());
  EXPECT_FALSE(r.h.strict());
  EXPECT_EQ(r.h.state(), FeedState::Live);
  EXPECT_EQ(r.last().cross, 'X');
  EXPECT_EQ(r.last().session, 0);
  EXPECT_EQ(r.h.counters().crossed_updates, 1U);
}

TEST(TradingPhase, PreMarketAllowsLockedAndCrossedBooks) {
  Script r;
  r << tk::system_event('O', r.t()) << tk::stock_directory(kLoc, "ACME", r.t()) << tk::system_event('S', r.t())
    << halt('T', r.t()) << add(1, 'B', 100, 10000, r.t()) << add(2, 'S', 10, 10000, r.t());
  EXPECT_FALSE(r.h.strict()) << "session 'S' is before market hours";
  EXPECT_EQ(r.last().kind, FeedEvent::Kind::BookUpdate);
  EXPECT_EQ(r.last().cross, 'L');
  EXPECT_FALSE(md::tradable(r.last()));
  r << add(3, 'S', 10, 9990, r.t());
  EXPECT_EQ(r.last().cross, 'X');
  EXPECT_EQ(r.h.counters().locked_updates, 1U);
  EXPECT_EQ(r.h.counters().crossed_updates, 1U);
  EXPECT_EQ(r.h.state(), FeedState::Live);
}

TEST(TradingPhase, MarketOpenWithACleanBookBecomesStrict) {
  Script r;
  open_market(r);
  EXPECT_TRUE(md::tradable(r.last()));
  r << add(3, 'S', 10, 9990, r.t());  // would cross in regular trading
  EXPECT_EQ(r.h.state(), FeedState::Stale);
  EXPECT_EQ(r.last().reason, AlertReason::BookRejected);
  EXPECT_EQ(r.last().detail, static_cast<std::uint8_t>(md::BookStatus::WouldCross));
}

TEST(TradingPhase, MarketOpenOnACrossedBookGoesStale) {
  Script r;
  r << tk::system_event('S', r.t()) << tk::stock_directory(kLoc, "ACME", r.t()) << halt('T', r.t())
    << add(1, 'B', 100, 10000, r.t()) << add(2, 'S', 10, 9990, r.t()) << tk::system_event('Q', r.t());
  EXPECT_EQ(r.h.state(), FeedState::Stale);
  EXPECT_EQ(r.last().reason, AlertReason::CrossedAtResume);
  EXPECT_EQ(r.last().detail, 'X');
}

TEST(TradingPhase, MarketOpenOnALockedBookGoesStale) {
  Script r;
  r << tk::system_event('S', r.t()) << tk::stock_directory(kLoc, "ACME", r.t()) << halt('T', r.t())
    << add(1, 'B', 100, 10000, r.t()) << add(2, 'S', 10, 10000, r.t()) << tk::system_event('Q', r.t());
  EXPECT_EQ(r.h.state(), FeedState::Stale);
  EXPECT_EQ(r.last().reason, AlertReason::CrossedAtResume);
  EXPECT_EQ(r.last().detail, 'L');
}

TEST(TradingPhase, RegularHoursWithoutATradingActionAreNotStrict) {
  Script r;
  r << tk::stock_directory(kLoc, "ACME", r.t()) << tk::system_event('Q', r.t()) << add(1, 'B', 1, 10000, r.t())
    << add(2, 'S', 1, 9000, r.t());
  EXPECT_FALSE(r.h.strict()) << "the instrument's state is unknown: no Trading Action yet";
  EXPECT_EQ(r.h.state(), FeedState::Live);
}

TEST(TradingPhase, HaltRelaxesTheCheckAndFlagsEvents) {
  Script r;
  open_market(r);
  r << halt('H', r.t());
  EXPECT_EQ(r.last().kind, FeedEvent::Kind::Trading);
  EXPECT_EQ(r.last().detail, 'H');
  EXPECT_FALSE(r.h.strict());
  EXPECT_FALSE(md::tradable(r.last()));
  r << add(3, 'S', 30, 9990, r.t());
  EXPECT_EQ(r.h.state(), FeedState::Live);
  EXPECT_EQ(r.last().kind, FeedEvent::Kind::BookUpdate);
  EXPECT_EQ(r.last().cross, 'X');
  EXPECT_EQ(r.last().trading, 'H');
  EXPECT_EQ(r.h.counters().crossed_updates, 1U);
  EXPECT_EQ(r.h.book().find_invariant_violation(), std::nullopt);
}

TEST(TradingPhase, PauseRelaxesTheCheck) {
  Script r;
  open_market(r);
  r << halt('P', r.t()) << add(3, 'S', 30, 10000, r.t());
  EXPECT_EQ(r.h.state(), FeedState::Live);
  EXPECT_EQ(r.last().cross, 'L');
}

TEST(TradingPhase, QuotationOnlyRelaxesTheCheck) {
  Script r;
  open_market(r);
  r << halt('H', r.t()) << halt('Q', r.t()) << add(3, 'B', 30, 10200, r.t());
  EXPECT_EQ(r.h.state(), FeedState::Live);
  EXPECT_EQ(r.h.trading_state(), 'Q');
  EXPECT_EQ(r.last().cross, 'X');
}

// The complete sequence: trading -> halt -> the book crosses during the
// halt -> the halt cross executes the crossing interest -> the book is
// clean -> trading resumes (strict again) -> a crossing add is rejected.
TEST(TradingPhase, CrossedDuringAHaltThenCleanWhenTradingResumes) {
  Script r;
  open_market(r);                                             // seq 1-7: bid 10000x100, ask 10100x100
  r << halt('H', r.t());                                      // seq 8: halted
  r << add(3, 'S', 60, 9950, r.t());                          // seq 9: sell below the bid: crossed
  r << add(4, 'B', 40, 10120, r.t());                         // seq 10: buy above the ask: still crossed
  r << tk::order_executed(kLoc, 3, 60, 1, r.t());             // seq 11: halt cross: sell 3 fully executed
  r << tk::order_executed(kLoc, 4, 40, 2, r.t());             // seq 12: buy 4 fully executed
  r << halt('T', r.t());                                      // seq 13: trading resumes on a clean book
  r << add(5, 'S', 10, 9990, r.t());                          // seq 14: crossing add in regular trading
  const std::string expected =
      "seq=1 ts=1 SYSTEM state=UNINITIALIZED reason=- detail=79 type=S ref=0 bid=0x0 ask=0x0 sess=O trd=- cross=-\n"
      "seq=2 ts=2 STATE state=LIVE reason=- detail=0 type=R ref=0 bid=0x0 ask=0x0 sess=O trd=- cross=-\n"
      "seq=3 ts=3 SYSTEM state=LIVE reason=- detail=83 type=S ref=0 bid=0x0 ask=0x0 sess=S trd=- cross=-\n"
      "seq=4 ts=4 TRADING state=LIVE reason=- detail=84 type=H ref=0 bid=0x0 ask=0x0 sess=S trd=T cross=-\n"
      "seq=5 ts=5 SYSTEM state=LIVE reason=- detail=81 type=S ref=0 bid=0x0 ask=0x0 sess=Q trd=T cross=-\n"
      "seq=6 ts=6 BOOK state=LIVE reason=- detail=0 type=A ref=1 bid=10000x100 ask=0x0 msg=B10000x100 sess=Q trd=T cross=-\n"
      "seq=7 ts=7 BOOK state=LIVE reason=- detail=0 type=A ref=2 bid=10000x100 ask=10100x100 msg=S10100x100 sess=Q trd=T cross=-\n"
      "seq=8 ts=8 TRADING state=LIVE reason=- detail=72 type=H ref=0 bid=10000x100 ask=10100x100 sess=Q trd=H cross=-\n"
      "seq=9 ts=9 BOOK state=LIVE reason=- detail=0 type=A ref=3 bid=10000x100 ask=9950x60 msg=S9950x60 sess=Q trd=H cross=X\n"
      "seq=10 ts=10 BOOK state=LIVE reason=- detail=0 type=A ref=4 bid=10120x40 ask=9950x60 msg=B10120x40 sess=Q trd=H cross=X\n"
      "seq=11 ts=11 BOOK state=LIVE reason=- detail=0 type=E ref=3 bid=10120x40 ask=10100x100 msg=S9950x60 sess=Q trd=H cross=X\n"
      "seq=12 ts=12 BOOK state=LIVE reason=- detail=0 type=E ref=4 bid=10000x100 ask=10100x100 msg=B10120x40 sess=Q trd=H cross=-\n"
      "seq=13 ts=13 TRADING state=LIVE reason=- detail=84 type=H ref=0 bid=10000x100 ask=10100x100 sess=Q trd=T cross=-\n"
      "seq=14 ts=14 STATE state=STALE reason=BOOK_REJECTED detail=7 type=A ref=0 bid=10000x100 ask=10100x100 sess=Q trd=T cross=-\n";
  EXPECT_EQ(r.text, expected);
  EXPECT_EQ(r.h.counters().crossed_updates, 3U);
  EXPECT_EQ(r.h.counters().locked_updates, 0U);
  EXPECT_EQ(r.h.state(), FeedState::Stale) << "strict mode really is back on after the resume";
}

TEST(TradingPhase, ResumingWhileStillCrossedGoesStale) {
  Script r;
  open_market(r);
  r << halt('H', r.t()) << add(3, 'S', 60, 9950, r.t()) << halt('T', r.t());
  EXPECT_EQ(r.h.state(), FeedState::Stale);
  EXPECT_EQ(r.last().reason, AlertReason::CrossedAtResume);
  EXPECT_EQ(r.last().detail, 'X');
}

TEST(TradingPhase, MarketCloseRelaxesTheCheck) {
  Script r;
  open_market(r);
  r << tk::system_event('M', r.t());
  EXPECT_FALSE(r.h.strict());
  EXPECT_FALSE(md::tradable(r.last()));
  r << add(3, 'S', 10, 9900, r.t());
  EXPECT_EQ(r.h.state(), FeedState::Live);
  EXPECT_EQ(r.last().cross, 'X');
}

TEST(TradingPhase, ATradingActionForAnotherInstrumentIsIgnored) {
  Script r;
  open_market(r);
  r << tk::trading_action(9, "ZZZZ", 'H', r.t());
  EXPECT_TRUE(r.h.strict());
  EXPECT_EQ(r.h.trading_state(), 'T');
  EXPECT_EQ(r.h.counters().other_stock, 1U);
}

TEST(TradingPhase, PhaseIsFollowedWhileStale) {
  Script r;
  open_market(r);
  r << tk::order_delete(kLoc, 99, r.t());  // unknown ref -> Stale
  ASSERT_EQ(r.h.state(), FeedState::Stale);
  r << halt('H', r.t());
  EXPECT_EQ(r.h.trading_state(), 'H');
  EXPECT_FALSE(r.h.strict());
  r << tk::system_event('M', r.t());
  EXPECT_EQ(r.h.session(), 'M');
}

TEST(TradingPhase, TradableNeedsLiveRegularHoursTradingAndACleanBook) {
  FeedEvent e{};
  e.state = FeedState::Live;
  e.session = 'Q';
  e.trading = 'T';
  e.cross = '-';
  EXPECT_TRUE(md::tradable(e));
  for (const char s : std::string("OSMEC")) {
    FeedEvent x = e;
    x.session = s;
    EXPECT_FALSE(md::tradable(x)) << s;
  }
  for (const char t : std::string("HPQ")) {
    FeedEvent x = e;
    x.trading = t;
    EXPECT_FALSE(md::tradable(x)) << t;
  }
  for (const char c : std::string("LX")) {
    FeedEvent x = e;
    x.cross = c;
    EXPECT_FALSE(md::tradable(x)) << c;
  }
  FeedEvent x = e;
  x.state = FeedState::Stale;
  EXPECT_FALSE(md::tradable(x));
}

}  // namespace
