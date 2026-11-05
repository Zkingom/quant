// L4.3: simulated exchange core (sans-IO). One named test per behaviour of
// the OUCH 4.2 specification and of the simulator's subset policy.
// L4 design §2.4, §2.6.

#include "lob/net/ouch_exchange.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "lob/testkit/itch_builder.hpp"
#include "lob/testkit/itch_samples.hpp"

namespace {

namespace md = lob::md;
namespace net = lob::net;
namespace ouch = lob::ouch;
namespace tk = lob::testkit::itch;
namespace samples = lob::testkit::itch_samples;
using samples::kLocate;

constexpr std::uint64_t kSec = 1'000'000'000ULL;

struct Ex {
  net::OuchExchange ex;
  std::uint64_t seq{0};
  std::vector<std::uint64_t> read_upto;  // per account: next sequence number not yet read

  explicit Ex(net::ExchangeConfig cfg = make_config()) : ex(cfg), read_upto(cfg.accounts.size(), 1) {
    // Market open, ACME trading; a resting bid 100 @ 1.0000 (ref 1) and ask 200 @ 1.0100 (ref 3).
    for (const auto& m : {tk::system_event('O', 1), tk::stock_directory(kLocate, "ACME", 3),
                          tk::trading_action(kLocate, "ACME", 'T', 4), tk::system_event('Q', 5),
                          tk::add_order(kLocate, 1, 'B', 100, "ACME", 10000, 10),
                          tk::add_order(kLocate, 3, 'S', 200, "ACME", 10100, 12)}) {
      market(m);
    }
    for (std::size_t a = 0; a < read_upto.size(); ++a) (void)out(a);  // skip Start of Day
  }

  static net::ExchangeConfig make_config() {
    net::ExchangeConfig c;
    c.feed = samples::config();
    c.accounts = {net::AccountConfig{"A1", "pw", "FIRM", true, 0, 1}, net::AccountConfig{"B1", "pw", "BBBB", false, 0, 1}};
    c.log_messages = 1000;
    c.log_bytes = 1U << 16;
    c.max_orders = 64;
    return c;
  }

  void market(const tk::Buf& payload) {
    md::FeedInput in;
    in.seq = ++seq;
    in.kind = md::InputKind::Decoded;
    EXPECT_EQ(md::itch::decode(payload, in.msg), md::itch::DecodeStatus::Decoded);
    ex.on_market(in);
  }

  template <class M>
  void send(std::size_t a, const M& m, std::uint64_t now = 0) {
    std::array<std::uint8_t, ouch::kMaxMessageSize> b{};
    const std::size_t n = ouch::encode(m, b);
    ex.on_message(a, {b.data(), n}, now);
  }

  /// Outbound messages of account `a` not read yet.
  std::vector<ouch::Outbound> out(std::size_t a) {
    std::vector<ouch::Outbound> r;
    const auto& log = ex.log(a);
    for (; read_upto[a] < log.next_sequence(); ++read_upto[a]) {
      ouch::Outbound m;
      EXPECT_EQ(ouch::decode_outbound(log.at(read_upto[a]), m), ouch::DecodeStatus::Decoded);
      r.push_back(m);
    }
    return r;
  }
};

ouch::EnterOrder enter(std::string_view token, ouch::Side side, std::uint32_t shares, std::uint32_t price,
                       std::uint32_t tif = ouch::kTifSystemHours) {
  ouch::EnterOrder e;
  e.token = ouch::make_token(token);
  e.side = side;
  e.shares = shares;
  e.stock = ouch::make_symbol("ACME");
  e.price = price;
  e.time_in_force = tif;
  e.firm = ouch::alpha<4>("");
  return e;
}

ouch::ReplaceOrder replace(std::string_view existing, std::string_view replacement, std::uint32_t shares,
                           std::uint32_t price) {
  return ouch::ReplaceOrder{ouch::make_token(existing), ouch::make_token(replacement), shares, price,
                            ouch::kTifSystemHours, 'A', 'N', 0};
}

ouch::CancelOrder cancel(std::string_view token, std::uint32_t intended) {
  return ouch::CancelOrder{ouch::make_token(token), intended};
}

template <class M>
const M& as(const ouch::Outbound& o) {
  EXPECT_TRUE(std::holds_alternative<M>(o));
  static const M empty{};
  return std::holds_alternative<M>(o) ? std::get<M>(o) : empty;
}

std::string tok(const ouch::Token& t) {
  std::string s(t.data(), t.size());
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s;
}

char reject_reason(Ex& x, std::size_t a = 0) {
  const auto o = x.out(a);
  EXPECT_EQ(o.size(), 1U);
  return o.size() == 1 ? as<ouch::Rejected>(o[0]).reason : '?';
}

// --- Start / end of day ------------------------------------------------------------

TEST(OuchExchange, StartOfDayIsTheFirstMessageOfEveryAccount) {
  net::OuchExchange ex(Ex::make_config());
  for (std::size_t a = 0; a < ex.accounts(); ++a) {
    ASSERT_EQ(ex.log(a).next_sequence(), 2U);
    ouch::Outbound m;
    ASSERT_EQ(ouch::decode_outbound(ex.log(a).at(1), m), ouch::DecodeStatus::Decoded);
    EXPECT_EQ(as<ouch::SystemEvent>(m).event_code, 'S');
  }
}

TEST(OuchExchange, EndOfDaySendsSystemEventEAndEndsTheSessions) {
  Ex x;
  x.ex.end_of_day();
  const auto o = x.out(0);
  ASSERT_EQ(o.size(), 1U);
  EXPECT_EQ(as<ouch::SystemEvent>(o[0]).event_code, 'E');
  EXPECT_TRUE(x.ex.log(0).ended());
  EXPECT_TRUE(x.ex.log(1).ended());
}

// --- Enter Order ---------------------------------------------------------------------

TEST(OuchExchange, Enter_Valid_AcceptedWithReferenceNumberAndEchoedFields) {
  Ex x;
  x.send(0, enter("T1", ouch::Side::Buy, 100, 9900));
  const auto o = x.out(0);
  ASSERT_EQ(o.size(), 1U);
  const auto& a = as<ouch::Accepted>(o[0]);
  EXPECT_EQ(tok(a.token), "T1");
  EXPECT_EQ(a.side, ouch::Side::Buy);
  EXPECT_EQ(a.shares, 100U);
  EXPECT_EQ(a.price, 9900U);
  EXPECT_EQ(std::string(a.firm.data(), 4), "FIRM");  // blank firm -> the account's default
  EXPECT_EQ(a.order_state, 'L');
  EXPECT_EQ(a.order_ref, 1U);
  EXPECT_EQ(a.timestamp, 12U);  // the exchange's market time
  EXPECT_EQ(x.ex.position(0).open_buy, 100U);
  EXPECT_EQ(x.ex.position(0).live_orders, 1U);
}

TEST(OuchExchange, Enter_DuplicateToken_IsIgnored) {
  Ex x;
  x.send(0, enter("T1", ouch::Side::Buy, 100, 9900));
  (void)x.out(0);
  x.send(0, enter("T1", ouch::Side::Sell, 5, 20000));  // "the new order will be ignored"
  EXPECT_TRUE(x.out(0).empty());
  EXPECT_EQ(x.ex.counters().duplicate_tokens, 1U);
  EXPECT_EQ(x.ex.position(0).live_orders, 1U);
}

TEST(OuchExchange, Enter_TokensAreUniquePerAccountOnly) {
  Ex x;
  x.send(0, enter("T1", ouch::Side::Buy, 100, 9900));
  x.send(1, enter("T1", ouch::Side::Buy, 100, 9900));
  EXPECT_EQ(x.out(0).size(), 1U);
  ASSERT_EQ(x.out(1).size(), 1U);
  EXPECT_EQ(x.ex.position(1).live_orders, 1U);
}

TEST(OuchExchange, Enter_SubsetPolicy_RejectsWithSpecReasons) {
  Ex x;
  auto other = enter("R1", ouch::Side::Buy, 10, 9900);
  other.stock = ouch::make_symbol("ZZZZ");
  x.send(0, other);
  EXPECT_EQ(reject_reason(x), 'S');
  x.send(0, enter("R2", ouch::Side::SellShort, 10, 9900));
  EXPECT_EQ(reject_reason(x), 'O');
  x.send(0, enter("R3", ouch::Side::Buy, 10, ouch::kMarketPrice));
  EXPECT_EQ(reject_reason(x), 'X');
  x.send(0, enter("R4", ouch::Side::Buy, 10, 0));
  EXPECT_EQ(reject_reason(x), 'X');
  auto nd = enter("R5", ouch::Side::Buy, 10, 9900);
  nd.display = 'N';
  x.send(0, nd);
  EXPECT_EQ(reject_reason(x), 'O');
  x.send(0, enter("R6", ouch::Side::Buy, 10, 9900, 5));  // TIF 5 seconds: not supported
  EXPECT_EQ(reject_reason(x), 'O');
  auto mq = enter("R7", ouch::Side::Buy, 10, 9900);
  mq.min_qty = 5;
  x.send(0, mq);
  EXPECT_EQ(reject_reason(x), 'N');
  auto cross = enter("R8", ouch::Side::Buy, 10, 9900);
  cross.cross_type = 'O';
  x.send(0, cross);
  EXPECT_EQ(reject_reason(x), 'R');
  EXPECT_EQ(x.ex.position(0).live_orders, 0U);
}

TEST(OuchExchange, Enter_RejectedTokenCannotBeReused) {
  Ex x;
  x.send(0, enter("R1", ouch::Side::Buy, 10, 0));
  EXPECT_EQ(reject_reason(x), 'X');
  x.send(0, enter("R1", ouch::Side::Buy, 10, 9900));
  EXPECT_TRUE(x.out(0).empty());  // consumed by the rejection
}

TEST(OuchExchange, Enter_ImmediateOrCancel_RemainderCanceledReasonI) {
  Ex x;
  x.send(0, enter("I1", ouch::Side::Buy, 100, 9900, ouch::kTifImmediateOrCancel));
  const auto o = x.out(0);
  ASSERT_EQ(o.size(), 2U);
  EXPECT_EQ(as<ouch::Accepted>(o[0]).shares, 100U);
  EXPECT_EQ(as<ouch::Canceled>(o[1]).decrement, 100U);
  EXPECT_EQ(as<ouch::Canceled>(o[1]).reason, 'I');
  EXPECT_EQ(x.ex.position(0).live_orders, 0U);
}

TEST(OuchExchange, InvalidOrOutOfSubsetInbound_IsCountedAndIgnored) {
  Ex x;
  const std::uint8_t garbage[] = {'Q', 1, 2, 3};
  x.ex.on_message(0, garbage, 0);
  std::uint8_t modify[20] = {'M'};  // Modify Order: valid spec type, outside the subset
  x.ex.on_message(0, modify, 0);
  EXPECT_TRUE(x.out(0).empty());
  EXPECT_EQ(x.ex.counters().invalid_inbound, 2U);
}

// --- Executions -------------------------------------------------------------------------

TEST(OuchExchange, Market_ExecutionAtOurPrice_ProducesExecutedAndPosition) {
  Ex x;  // optimistic model: we are assumed at the front of the queue
  x.send(0, enter("B1", ouch::Side::Buy, 100, 10000));
  (void)x.out(0);
  x.market(tk::order_executed(kLocate, 1, 40, 77, 20));  // the market sold 40 into the 1.0000 bid
  const auto o = x.out(0);
  ASSERT_EQ(o.size(), 1U);
  const auto& e = as<ouch::Executed>(o[0]);
  EXPECT_EQ(tok(e.token), "B1");
  EXPECT_EQ(e.shares, 40U);
  EXPECT_EQ(e.price, 10000U);
  EXPECT_EQ(e.liquidity, 'A');
  EXPECT_EQ(e.match_number, 1U);
  EXPECT_EQ(e.timestamp, 20U);
  const auto p = x.ex.position(0);
  EXPECT_EQ(p.position, 40);
  EXPECT_EQ(p.cash, -40 * 10000);
  EXPECT_EQ(p.open_buy, 60U);
}

// --- Cancel Order -------------------------------------------------------------------------

TEST(OuchExchange, Cancel_ZeroIntendedSize_CancelsEverythingReasonU) {
  Ex x;
  x.send(0, enter("C1", ouch::Side::Buy, 100, 9900));
  (void)x.out(0);
  x.send(0, cancel("C1", 0));
  const auto o = x.out(0);
  ASSERT_EQ(o.size(), 1U);
  EXPECT_EQ(as<ouch::Canceled>(o[0]).decrement, 100U);
  EXPECT_EQ(as<ouch::Canceled>(o[0]).reason, 'U');
  EXPECT_EQ(x.ex.position(0).live_orders, 0U);
}

TEST(OuchExchange, Cancel_Repeated_IsHarmless) {
  Ex x;
  x.send(0, enter("C1", ouch::Side::Buy, 100, 9900));
  x.send(0, cancel("C1", 0));
  (void)x.out(0);
  x.send(0, cancel("C1", 0));  // benign resend after a reconnect
  EXPECT_TRUE(x.out(0).empty());
  EXPECT_EQ(x.ex.counters().ignored_cancels, 1U);
}

TEST(OuchExchange, Cancel_IntendedSizeNotBelowOpen_IsSuperfluousAndIgnored) {
  Ex x;
  x.send(0, enter("C1", ouch::Side::Buy, 100, 9900));
  (void)x.out(0);
  x.send(0, cancel("C1", 100));
  x.send(0, cancel("C1", 500));
  EXPECT_TRUE(x.out(0).empty());
  EXPECT_EQ(x.ex.position(0).open_buy, 100U);
  EXPECT_EQ(x.ex.counters().ignored_cancels, 2U);
}

TEST(OuchExchange, Cancel_Partial_ReducesWithIncrementalDecrements) {
  Ex x;
  x.send(0, enter("C1", ouch::Side::Buy, 100, 9900));
  (void)x.out(0);
  x.send(0, cancel("C1", 30));
  x.send(0, cancel("C1", 30));  // same intended size again: nothing to do
  x.send(0, cancel("C1", 10));
  const auto o = x.out(0);
  ASSERT_EQ(o.size(), 2U);
  EXPECT_EQ(as<ouch::Canceled>(o[0]).decrement, 70U);
  EXPECT_EQ(as<ouch::Canceled>(o[1]).decrement, 20U);
  EXPECT_EQ(x.ex.position(0).open_buy, 10U);
  EXPECT_EQ(x.ex.position(0).live_orders, 1U);
}

TEST(OuchExchange, Cancel_Partial_KeepsThePlaceInThePublicQueue) {
  auto cfg = Ex::make_config();
  cfg.sim.model = lob::sim::FillModel::Conservative;
  Ex x(cfg);
  x.send(0, enter("C1", ouch::Side::Buy, 100, 10000));  // behind the resting 100 at 1.0000
  x.send(0, cancel("C1", 30));
  (void)x.out(0);
  const auto find = [&]() -> const lob::sim::OpenOrder* {
    for (const auto& o : x.ex.sim().open_orders()) {
      if (o.id != 0) return &o;
    }
    return nullptr;
  };
  ASSERT_NE(find(), nullptr);
  EXPECT_EQ(find()->leaves, 30U);
  EXPECT_EQ(find()->queue_ahead, 100U);  // not reset to the back of the queue
  EXPECT_EQ(find()->cutoff_ref, 1U);
  x.market(tk::order_executed(kLocate, 1, 100, 9, 20));  // the 100 ahead of us trade: nothing for us yet
  EXPECT_TRUE(x.out(0).empty());
  EXPECT_EQ(find()->queue_ahead, 0U);
}

TEST(OuchExchange, Cancel_UnknownToken_IsIgnored) {
  Ex x;
  x.send(0, cancel("NOPE", 0));
  EXPECT_TRUE(x.out(0).empty());
}

// --- Replace Order -------------------------------------------------------------------------

TEST(OuchExchange, Replace_Valid_ReplacedWithNewTokenReferenceAndPrevious) {
  Ex x;
  x.send(0, enter("P1", ouch::Side::Buy, 100, 9900));
  (void)x.out(0);
  x.send(0, replace("P1", "P2", 150, 9950));
  const auto o = x.out(0);
  ASSERT_EQ(o.size(), 1U);
  const auto& r = as<ouch::Replaced>(o[0]);
  EXPECT_EQ(tok(r.replacement), "P2");
  EXPECT_EQ(tok(r.previous), "P1");
  EXPECT_EQ(r.shares, 150U);
  EXPECT_EQ(r.price, 9950U);
  EXPECT_EQ(r.order_ref, 2U);  // a new reference number (priority lost)
  x.send(0, cancel("P1", 0));  // the old token is dead
  EXPECT_TRUE(x.out(0).empty());
  x.send(0, cancel("P2", 0));
  EXPECT_EQ(as<ouch::Canceled>(x.out(0).at(0)).decrement, 150U);
}

TEST(OuchExchange, Replace_SharesAreLiableForTheWholeChain) {
  Ex x;
  x.send(0, enter("P1", ouch::Side::Buy, 500, 10000));
  x.market(tk::order_executed(kLocate, 1, 100, 78, 20));  // 100 of ours execute
  (void)x.out(0);
  x.send(0, replace("P1", "P2", 500, 10000));  // spec §2.2 a.: keep 400 exposed
  const auto o = x.out(0);
  ASSERT_EQ(o.size(), 1U);
  EXPECT_EQ(as<ouch::Replaced>(o[0]).shares, 400U);
  x.send(0, replace("P2", "P3", 600, 10000));  // spec §2.2 b.: a new 500
  EXPECT_EQ(as<ouch::Replaced>(x.out(0).at(0)).shares, 500U);
}

TEST(OuchExchange, Replace_ExistingNotLiveOrReplacementUsed_IsIgnored) {
  Ex x;
  x.send(0, enter("P1", ouch::Side::Buy, 100, 9900));
  x.send(0, enter("P2", ouch::Side::Buy, 100, 9800));
  (void)x.out(0);
  x.send(0, replace("P1", "P2", 100, 9850));    // replacement token already used
  x.send(0, replace("NONE", "P9", 100, 9850));  // existing unknown
  EXPECT_TRUE(x.out(0).empty());
  EXPECT_EQ(x.ex.counters().ignored_replaces, 2U);
  EXPECT_EQ(x.ex.position(0).live_orders, 2U);
}

TEST(OuchExchange, Replace_InvalidDetails_CancelsExistingAndLeavesTokenUnused) {
  Ex x;
  x.send(0, enter("P1", ouch::Side::Buy, 100, 9900));
  (void)x.out(0);
  x.send(0, replace("P1", "P2", 100, 0));  // invalid price
  const auto o = x.out(0);
  ASSERT_EQ(o.size(), 1U);
  EXPECT_EQ(as<ouch::Canceled>(o[0]).decrement, 100U);
  EXPECT_EQ(x.ex.position(0).live_orders, 0U);
  x.send(0, enter("P2", ouch::Side::Buy, 10, 9900));  // "may be reused in this case"
  EXPECT_TRUE(std::holds_alternative<ouch::Accepted>(x.out(0).at(0)));
}

TEST(OuchExchange, Replace_SharesNotAboveExecuted_CancelsExisting) {
  Ex x;
  x.send(0, enter("P1", ouch::Side::Buy, 100, 10000));
  x.market(tk::order_executed(kLocate, 1, 40, 79, 20));
  (void)x.out(0);
  x.send(0, replace("P1", "P2", 40, 10000));  // liable 40, executed 40: nothing left to expose
  const auto o = x.out(0);
  ASSERT_EQ(o.size(), 1U);
  EXPECT_EQ(as<ouch::Canceled>(o[0]).decrement, 60U);
}

// --- Throttle -----------------------------------------------------------------------------

TEST(OuchExchange, Throttle_ExcessEntersRejectedX_CancelsNeverThrottled) {
  auto cfg = Ex::make_config();
  cfg.accounts[0].throttle_per_sec = 2;
  cfg.accounts[0].throttle_burst = 2;
  Ex x(cfg);
  x.send(0, enter("Q1", ouch::Side::Buy, 10, 9900), 0);
  x.send(0, enter("Q2", ouch::Side::Buy, 10, 9900), 0);
  x.send(0, enter("Q3", ouch::Side::Buy, 10, 9900), 0);
  auto o = x.out(0);
  ASSERT_EQ(o.size(), 3U);
  EXPECT_EQ(as<ouch::Rejected>(o[2]).reason, ouch::reason::kRejectPortRate);
  x.send(0, cancel("Q1", 0), 0);
  x.send(0, cancel("Q2", 0), 0);
  EXPECT_EQ(x.out(0).size(), 2U);  // both cancels processed with an empty bucket
  x.send(0, enter("Q4", ouch::Side::Buy, 10, 9900), kSec / 2);  // one token refilled after 0.5 s
  EXPECT_TRUE(std::holds_alternative<ouch::Accepted>(x.out(0).at(0)));
  x.send(0, enter("Q5", ouch::Side::Buy, 10, 9900), kSec / 2);
  EXPECT_EQ(reject_reason(x), 'x');
  EXPECT_EQ(x.ex.counters().throttled, 2U);
}

TEST(OuchExchange, Throttle_ReplaceOverLimit_RejectedAndExistingOrderIntact) {
  auto cfg = Ex::make_config();
  cfg.accounts[0].throttle_per_sec = 1;
  cfg.accounts[0].throttle_burst = 1;
  Ex x(cfg);
  x.send(0, enter("P1", ouch::Side::Buy, 100, 9900), 0);
  (void)x.out(0);
  x.send(0, replace("P1", "P2", 100, 9950), 0);
  EXPECT_EQ(reject_reason(x), 'x');
  EXPECT_EQ(x.ex.position(0).open_buy, 100U);
  x.send(0, replace("P1", "P2", 100, 9950), 2 * kSec);  // the replacement token was consumed
  EXPECT_TRUE(x.out(0).empty());
}

// --- Cancel on disconnect -------------------------------------------------------------------

TEST(OuchExchange, Disconnect_CancelsEveryLiveOrderInOrderReasonZ) {
  Ex x;
  x.send(0, enter("D1", ouch::Side::Buy, 100, 9900));
  x.send(0, enter("D2", ouch::Side::Sell, 50, 10200));
  x.send(0, enter("D3", ouch::Side::Buy, 10, 9800));
  x.send(0, cancel("D2", 0));
  (void)x.out(0);
  x.ex.on_disconnect(0);
  const auto o = x.out(0);
  ASSERT_EQ(o.size(), 2U);
  EXPECT_EQ(tok(as<ouch::Canceled>(o[0]).token), "D1");
  EXPECT_EQ(tok(as<ouch::Canceled>(o[1]).token), "D3");
  EXPECT_EQ(as<ouch::Canceled>(o[0]).reason, ouch::reason::kSystemCancel);
  EXPECT_EQ(x.ex.position(0).live_orders, 0U);
  EXPECT_EQ(x.ex.counters().cancelled_on_disconnect, 2U);
}

TEST(OuchExchange, Disconnect_WithoutCancelOnDisconnect_KeepsOrders) {
  Ex x;
  x.send(1, enter("K1", ouch::Side::Buy, 100, 9900));
  (void)x.out(1);
  x.ex.on_disconnect(1);
  EXPECT_TRUE(x.out(1).empty());
  EXPECT_EQ(x.ex.position(1).live_orders, 1U);
}

// --- Determinism ------------------------------------------------------------------------------

TEST(OuchExchange, SameCallsGiveIdenticalOutboundStreams) {
  auto run = [] {
    Ex x;
    x.send(0, enter("A", ouch::Side::Buy, 100, 10000));
    x.send(0, enter("B", ouch::Side::Sell, 100, 10100));
    x.market(tk::order_executed(kLocate, 1, 30, 1, 20));
    x.send(0, replace("A", "A2", 120, 10000));
    x.send(0, cancel("B", 40));
    x.market(tk::order_executed(kLocate, 3, 200, 2, 21));
    x.ex.on_disconnect(0);
    return std::pair{x.ex.outbound_hash(0), x.ex.log(0).next_sequence()};
  };
  const auto a = run();
  EXPECT_EQ(a, run());
  EXPECT_GT(a.second, 6U);
}

}  // namespace
