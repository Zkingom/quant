// L4.4: client order gateway. One named test per order-state transition
// (State_Event_Outcome), the reconnect rules, and the gateway against the
// simulated exchange core directly (no sockets). L4 design §2.3.

#include "lob/gateway/gateway.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "lob/net/ouch_exchange.hpp"
#include "lob/testkit/itch_builder.hpp"
#include "lob/testkit/itch_samples.hpp"

namespace {

namespace gw = lob::gateway;
namespace ouch = lob::ouch;
namespace sim = lob::sim;
namespace md = lob::md;
namespace net = lob::net;
namespace tk = lob::testkit::itch;
namespace samples = lob::testkit::itch_samples;
using gw::OrderState;
using Kind = lob::strategy::OrderRequest::Kind;
using Buf = std::vector<std::uint8_t>;

struct FakeTransport final : gw::Transport {
  std::vector<ouch::Inbound> sent;
  bool up{true};
  bool send(ouch::Bytes p) override {
    if (!up) return false;
    ouch::Inbound m;
    EXPECT_EQ(ouch::decode_inbound(p, m), ouch::DecodeStatus::Decoded);
    sent.push_back(m);
    return true;
  }
  template <class M>
  const M& last() const {
    EXPECT_FALSE(sent.empty());
    EXPECT_TRUE(std::holds_alternative<M>(sent.back()));
    static const M none{};
    return sent.empty() || !std::holds_alternative<M>(sent.back()) ? none : std::get<M>(sent.back());
  }
};

gw::GatewayConfig cfg() {
  gw::GatewayConfig c;
  c.symbol = ouch::make_symbol("ACME");
  c.day = "1105";
  c.instance = 1;
  c.max_orders = 64;
  return c;
}

lob::strategy::OrderRequest new_order(std::uint64_t id, sim::Side side, std::uint32_t price, std::uint32_t qty) {
  return lob::strategy::OrderRequest{Kind::New, id, side, price, qty};
}
lob::strategy::OrderRequest cancel_order(std::uint64_t id) { return lob::strategy::OrderRequest{Kind::Cancel, id, sim::Side::Buy, 0, 0}; }

template <class M>
Buf bytes(const M& m) {
  Buf b(ouch::kMaxMessageSize);
  b.resize(ouch::encode(m, b));
  return b;
}

Buf accepted(const ouch::Token& t, std::uint32_t shares, std::uint32_t price, char state = 'L') {
  ouch::Accepted a;
  a.token = t;
  a.shares = shares;
  a.stock = ouch::make_symbol("ACME");
  a.price = price;
  a.firm = ouch::alpha<4>("FIRM");  // value-initialized alpha fields are NULs: invalid on the wire
  a.order_ref = 77;
  a.order_state = state;
  return bytes(a);
}
Buf executed(const ouch::Token& t, std::uint32_t shares, std::uint32_t price) {
  return bytes(ouch::Executed{1, t, shares, price, 'A', 5});
}
Buf canceled(const ouch::Token& t, std::uint32_t dec, char reason = 'U') { return bytes(ouch::Canceled{1, t, dec, reason}); }
Buf rejected(const ouch::Token& t, char reason = 'O') { return bytes(ouch::Rejected{1, t, reason}); }
Buf replaced(const ouch::Token& t, const ouch::Token& prev, std::uint32_t shares, std::uint32_t price) {
  ouch::Replaced r;
  r.replacement = t;
  r.previous = prev;
  r.shares = shares;
  r.stock = ouch::make_symbol("ACME");
  r.price = price;
  r.firm = ouch::alpha<4>("FIRM");
  r.order_ref = 78;
  return bytes(r);
}

struct G {
  gw::OrderGateway g{cfg()};
  FakeTransport t;
  sim::Reports r;
  void submit(const lob::strategy::OrderRequest& q) {
    r.clear();
    g.submit(q, t, r);
  }
  void in(const Buf& b) {
    r.clear();
    g.on_message(b, r);
  }
  /// New order 1: buy 100 @ 9900, accepted.
  ouch::Token live() {
    submit(new_order(1, sim::Side::Buy, 9900, 100));
    const ouch::Token tok = g.find(1)->token;
    in(accepted(tok, 100, 9900));
    return tok;
  }
};

std::string str(const ouch::Token& t) { return std::string(t.data(), t.size()); }

// --- Tokens ------------------------------------------------------------------------

TEST(Gateway, TokenIsDayInstanceAndBase36Counter) {
  gw::OrderGateway g(cfg());
  EXPECT_EQ(str(g.token_for(1)), "11050100000001");
  EXPECT_EQ(str(g.token_for(35)), "1105010000000Z");
  EXPECT_EQ(str(g.token_for(36)), "11050100000010");
  auto c = cfg();
  c.instance = 36 * 2 + 5;
  EXPECT_EQ(str(gw::OrderGateway(c).token_for(1)).substr(4, 2), "25");
  EXPECT_TRUE(ouch::valid_token(g.token_for(123456789)));
}

TEST(Gateway, NextInstanceDiffersOnEveryStart) {
  const auto path = (std::filesystem::temp_directory_path() / "lob_gateway_instance_test").string();
  std::filesystem::remove(path);
  EXPECT_EQ(gw::next_instance(path), 0U);
  EXPECT_EQ(gw::next_instance(path), 1U);
  EXPECT_EQ(gw::next_instance(path), 2U);
  std::filesystem::remove(path);
}

// --- New order --------------------------------------------------------------------------

TEST(Gateway, Submit_New_SendsEnterOrderAndGoesPendingNew) {
  G x;
  x.submit(new_order(1, sim::Side::Sell, 10100, 50));
  EXPECT_EQ(x.r.size(), 0U);
  const auto& e = x.t.last<ouch::EnterOrder>();
  EXPECT_EQ(str(e.token), "11050100000001");
  EXPECT_EQ(e.side, ouch::Side::Sell);
  EXPECT_EQ(e.shares, 50U);
  EXPECT_EQ(e.price, 10100U);
  EXPECT_EQ(e.stock, ouch::make_symbol("ACME"));
  EXPECT_EQ(x.g.find(1)->state, OrderState::PendingNew);
}

TEST(Gateway, Submit_DuplicateOrZeroClientId_RejectedLocally) {
  G x;
  x.submit(new_order(1, sim::Side::Buy, 9900, 10));
  x.submit(new_order(1, sim::Side::Buy, 9900, 10));
  ASSERT_EQ(x.r.size(), 1U);
  EXPECT_EQ(x.r[0].kind, sim::Report::Kind::Rejected);
  x.submit(new_order(0, sim::Side::Buy, 9900, 10));
  EXPECT_EQ(x.r.size(), 1U);
  EXPECT_EQ(x.t.sent.size(), 1U);
}

TEST(Gateway, Submit_TransportRefuses_RejectedLocally) {
  G x;
  x.t.up = false;
  x.submit(new_order(1, sim::Side::Buy, 9900, 10));
  ASSERT_EQ(x.r.size(), 1U);
  EXPECT_EQ(x.r[0].kind, sim::Report::Kind::Rejected);
  EXPECT_EQ(x.r[0].qty, 10U);
  EXPECT_EQ(x.g.find(1)->state, OrderState::Rejected);
}

TEST(Gateway, PendingNew_AcceptedLive_GoesLiveWithoutReport) {
  G x;
  x.live();
  EXPECT_EQ(x.r.size(), 0U);
  EXPECT_EQ(x.g.find(1)->state, OrderState::Live);
  EXPECT_EQ(x.g.find(1)->exchange_ref, 77U);
}

TEST(Gateway, PendingNew_AcceptedDead_GoesCanceledWithReport) {
  G x;
  x.submit(new_order(1, sim::Side::Buy, 9900, 100));
  x.in(accepted(x.g.find(1)->token, 100, 9900, 'D'));
  ASSERT_EQ(x.r.size(), 1U);
  EXPECT_EQ(x.r[0].kind, sim::Report::Kind::Cancelled);
  EXPECT_EQ(x.r[0].qty, 100U);
  EXPECT_EQ(x.g.find(1)->state, OrderState::Canceled);
}

TEST(Gateway, PendingNew_Rejected_GoesRejectedWithReport) {
  G x;
  x.submit(new_order(1, sim::Side::Buy, 9900, 100));
  x.in(rejected(x.g.find(1)->token));
  ASSERT_EQ(x.r.size(), 1U);
  const sim::Report& rep = x.r[0];
  EXPECT_EQ(rep.kind, sim::Report::Kind::Rejected);
  EXPECT_EQ(rep.client_id, 1U);
  EXPECT_EQ(rep.qty, 100U);  // as the in-process SimExchange reports it
  EXPECT_EQ(rep.leaves, 0U);
  EXPECT_EQ(x.g.find(1)->state, OrderState::Rejected);
}

// --- Executions and cancels -----------------------------------------------------------------

TEST(Gateway, Live_PartialExecution_StaysLiveWithFillReport) {
  G x;
  const auto tok = x.live();
  x.in(executed(tok, 30, 9900));
  ASSERT_EQ(x.r.size(), 1U);
  EXPECT_EQ(x.r[0].kind, sim::Report::Kind::Fill);
  EXPECT_EQ(x.r[0].qty, 30U);
  EXPECT_EQ(x.r[0].price, 9900U);
  EXPECT_EQ(x.r[0].leaves, 70U);
  EXPECT_EQ(x.g.find(1)->state, OrderState::Live);
}

TEST(Gateway, Live_FullExecution_GoesFilled) {
  G x;
  const auto tok = x.live();
  x.in(executed(tok, 60, 9900));
  x.in(executed(tok, 40, 9890));
  ASSERT_EQ(x.r.size(), 1U);
  EXPECT_EQ(x.r[0].leaves, 0U);
  EXPECT_EQ(x.r[0].price, 9890U);
  EXPECT_EQ(x.g.find(1)->state, OrderState::Filled);
  EXPECT_FALSE(x.g.inconsistent());
}

TEST(Gateway, Live_Cancel_SendsIntendedSizeZeroThenCanceledReport) {
  G x;
  const auto tok = x.live();
  x.submit(cancel_order(1));
  const auto& c = x.t.last<ouch::CancelOrder>();
  EXPECT_EQ(c.token, tok);
  EXPECT_EQ(c.shares, 0U);
  EXPECT_TRUE(x.g.find(1)->cancel_requested);
  x.in(canceled(tok, 100));
  ASSERT_EQ(x.r.size(), 1U);
  EXPECT_EQ(x.r[0].kind, sim::Report::Kind::Cancelled);
  EXPECT_EQ(x.r[0].qty, 100U);
  EXPECT_EQ(x.r[0].leaves, 0U);
  EXPECT_EQ(x.g.find(1)->state, OrderState::Canceled);
}

TEST(Gateway, Live_CancelTwice_SendsOnce) {
  G x;
  x.live();
  x.submit(cancel_order(1));
  x.submit(cancel_order(1));
  EXPECT_EQ(x.g.counters().sent_cancel, 1U);
}

TEST(Gateway, CancelOfUnknownOrFinalOrder_SendsNothing) {
  G x;
  x.submit(cancel_order(42));
  const auto tok = x.live();
  x.in(executed(tok, 100, 9900));
  const std::size_t before = x.t.sent.size();
  x.submit(cancel_order(1));
  EXPECT_EQ(x.t.sent.size(), before);
}

TEST(Gateway, CancelRequested_ExecutionStillArrives_IsApplied) {
  G x;
  const auto tok = x.live();
  x.submit(cancel_order(1));
  x.in(executed(tok, 40, 9900));  // crossed with our cancel on the wire
  EXPECT_EQ(x.r[0].kind, sim::Report::Kind::Fill);
  x.in(canceled(tok, 60));
  EXPECT_EQ(x.r[0].qty, 60U);
  EXPECT_EQ(x.g.find(1)->state, OrderState::Canceled);
  EXPECT_FALSE(x.g.inconsistent());
}

TEST(Gateway, Live_UnsolicitedPartialCanceled_ReducesLeaves) {
  G x;
  const auto tok = x.live();
  x.in(canceled(tok, 25, 'S'));  // e.g. supervisory reduction
  ASSERT_EQ(x.r.size(), 1U);
  EXPECT_EQ(x.r[0].leaves, 75U);
  EXPECT_EQ(x.g.find(1)->state, OrderState::Live);
}

TEST(Gateway, PendingNew_CancelBeforeAccepted_SendsCancelAtOnce) {
  G x;
  x.submit(new_order(1, sim::Side::Buy, 9900, 100));
  x.submit(cancel_order(1));  // the exchange processes Enter then Cancel in order
  EXPECT_TRUE(std::holds_alternative<ouch::CancelOrder>(x.t.sent.back()));
  const auto tok = x.g.find(1)->token;
  x.in(accepted(tok, 100, 9900));
  x.in(canceled(tok, 100));
  EXPECT_EQ(x.g.find(1)->state, OrderState::Canceled);
}

// --- Replace ----------------------------------------------------------------------------------

TEST(Gateway, Live_Replace_SendsReplaceAndReplacedSwitchesToken) {
  G x;
  const auto tok = x.live();
  ASSERT_TRUE(x.g.replace(1, 150, 9950, x.t));
  const auto& u = x.t.last<ouch::ReplaceOrder>();
  EXPECT_EQ(u.existing, tok);
  EXPECT_EQ(str(u.replacement), "11050100000002");
  EXPECT_EQ(u.shares, 150U);
  x.in(replaced(u.replacement, tok, 150, 9950));
  EXPECT_EQ(x.r.size(), 0U);
  EXPECT_EQ(x.g.find(1)->token, u.replacement);
  EXPECT_EQ(x.g.find(1)->leaves, 150U);
  EXPECT_EQ(x.g.find(1)->price, 9950U);
  x.in(executed(tok, 10, 9900));  // the old token is dead
  EXPECT_TRUE(x.g.inconsistent());
}

TEST(Gateway, ReplacePending_RejectedReplacement_OrderUnchanged) {
  G x;
  const auto tok = x.live();
  ASSERT_TRUE(x.g.replace(1, 150, 9950, x.t));
  x.in(rejected(x.t.last<ouch::ReplaceOrder>().replacement, 'x'));
  EXPECT_EQ(x.r.size(), 0U);
  EXPECT_EQ(x.g.find(1)->token, tok);
  EXPECT_FALSE(x.g.find(1)->replace_pending);
  EXPECT_EQ(x.g.find(1)->state, OrderState::Live);
}

TEST(Gateway, ReplacePending_CanceledExistingToken_GoesCanceled) {
  G x;
  const auto tok = x.live();
  ASSERT_TRUE(x.g.replace(1, 150, 0, x.t));
  x.in(canceled(tok, 100));  // invalid replace: the exchange cancels the existing order
  EXPECT_EQ(x.r[0].kind, sim::Report::Kind::Cancelled);
  EXPECT_EQ(x.g.find(1)->state, OrderState::Canceled);
}

TEST(Gateway, ReplacePending_ExecutionOnExistingToken_IsApplied) {
  G x;
  const auto tok = x.live();
  ASSERT_TRUE(x.g.replace(1, 150, 9950, x.t));
  const auto rep = x.t.last<ouch::ReplaceOrder>().replacement;
  x.in(executed(tok, 30, 9900));
  EXPECT_EQ(x.r[0].leaves, 70U);
  x.in(replaced(rep, tok, 120, 9950));  // 150 liable - 30 executed
  EXPECT_EQ(x.g.find(1)->leaves, 120U);
  EXPECT_EQ(x.g.find(1)->executed, 30U);
  EXPECT_FALSE(x.g.inconsistent());
}

TEST(Gateway, CancelWhileReplacePending_CancelsBothTokens) {
  G x;
  const auto tok = x.live();
  ASSERT_TRUE(x.g.replace(1, 150, 9950, x.t));
  const auto rep = x.t.last<ouch::ReplaceOrder>().replacement;
  x.submit(cancel_order(1));
  ASSERT_GE(x.t.sent.size(), 2U);
  EXPECT_EQ(std::get<ouch::CancelOrder>(x.t.sent[x.t.sent.size() - 2]).token, tok);
  EXPECT_EQ(std::get<ouch::CancelOrder>(x.t.sent.back()).token, rep);
}

// --- Illegal transitions -------------------------------------------------------------------------

TEST(Gateway, IllegalTransitions_MakeTheGatewayInconsistent) {
  {
    G x;
    x.in(executed(ouch::make_token("SOMEONEELSE"), 1, 1));  // not our token
    EXPECT_TRUE(x.g.inconsistent());
  }
  {
    G x;
    const auto tok = x.live();
    x.in(executed(tok, 101, 9900));  // more than open
    EXPECT_TRUE(x.g.inconsistent());
    EXPECT_EQ(x.r.size(), 0U);
  }
  {
    G x;
    const auto tok = x.live();
    x.in(accepted(tok, 100, 9900));  // accepted twice
    EXPECT_TRUE(x.g.inconsistent());
  }
  {
    G x;
    const auto tok = x.live();
    x.in(canceled(tok, 100));
    x.in(canceled(tok, 1));  // after the final state
    EXPECT_TRUE(x.g.inconsistent());
  }
  {
    G x;
    x.submit(new_order(1, sim::Side::Buy, 9900, 100));
    x.in(executed(x.g.find(1)->token, 10, 9900));  // executed before accepted
    EXPECT_TRUE(x.g.inconsistent());
  }
  {
    G x;
    x.in(Buf{'E', 1, 2});  // undecodable
    EXPECT_TRUE(x.g.inconsistent());
  }
  {
    G x;
    x.live();
    x.in(bytes(ouch::SystemEvent{0, 'S'}));  // system events are fine
    EXPECT_FALSE(x.g.inconsistent());
  }
}

// --- Reconnect ---------------------------------------------------------------------------------------

TEST(Gateway, Reconnect_ResendsPendingCancelsButNotNewOrdersByDefault) {
  G x;
  x.live();                                           // order 1 live
  x.submit(cancel_order(1));                          // cancel in flight
  x.submit(new_order(2, sim::Side::Sell, 10100, 5));  // new order in flight
  x.g.on_disconnected();
  EXPECT_TRUE(x.g.find(2)->in_doubt);
  const std::size_t before = x.t.sent.size();
  x.g.on_logged_in(x.t);
  ASSERT_EQ(x.t.sent.size(), before + 1);
  EXPECT_EQ(std::get<ouch::CancelOrder>(x.t.sent.back()).token, x.g.find(1)->token);  // same message again
  // The replay shows order 1 canceled; order 2 never reached the exchange.
  x.in(canceled(x.g.find(1)->token, 100));
  x.r.clear();
  x.g.on_replay_complete(x.r);
  ASSERT_EQ(x.r.size(), 1U);
  EXPECT_EQ(x.r[0].client_id, 2U);
  EXPECT_EQ(x.r[0].kind, sim::Report::Kind::Rejected);
  EXPECT_EQ(x.g.find(2)->state, OrderState::Rejected);
  EXPECT_EQ(x.g.counters().in_doubt_dropped, 1U);
}

TEST(Gateway, Reconnect_NewOrderAnsweredInTheReplay_IsNotDropped) {
  G x;
  x.submit(new_order(2, sim::Side::Sell, 10100, 5));
  x.g.on_disconnected();
  x.g.on_logged_in(x.t);
  x.in(accepted(x.g.find(2)->token, 5, 10100));  // it had arrived
  x.r.clear();
  x.g.on_replay_complete(x.r);
  EXPECT_EQ(x.r.size(), 0U);
  EXPECT_EQ(x.g.find(2)->state, OrderState::Live);
}

TEST(Gateway, Reconnect_WithResendNew_ResendsTheSameEnterOrder) {
  auto c = cfg();
  c.resend_new_on_reconnect = true;
  gw::OrderGateway g(c);
  FakeTransport t;
  sim::Reports r;
  g.submit(new_order(2, sim::Side::Sell, 10100, 5), t, r);
  g.on_disconnected();
  g.on_logged_in(t);
  ASSERT_EQ(t.sent.size(), 2U);
  EXPECT_EQ(std::get<ouch::EnterOrder>(t.sent[0]).token, std::get<ouch::EnterOrder>(t.sent[1]).token);
  g.on_replay_complete(r);
  EXPECT_EQ(g.find(2)->state, OrderState::PendingNew);  // its answer comes after the replay
}

TEST(Gateway, OpenIdsAreTheNonFinalOrdersAscending) {
  G x;
  for (std::uint64_t id : {5U, 3U, 9U, 4U}) x.submit(new_order(id, sim::Side::Buy, 9900, 10));
  x.in(rejected(x.g.find(9)->token));
  std::array<std::uint64_t, 8> ids{};
  const std::size_t n = x.g.open_ids(ids);
  EXPECT_EQ(std::vector<std::uint64_t>(ids.begin(), ids.begin() + static_cast<std::ptrdiff_t>(n)),
            (std::vector<std::uint64_t>{3, 4, 5}));
}

// --- Against the simulated exchange core, no sockets ----------------------------------------------------

/// Forwards every inbound message straight into the exchange.
struct DirectTransport final : gw::Transport {
  net::OuchExchange& ex;
  explicit DirectTransport(net::OuchExchange& e) : ex(e) {}
  bool send(ouch::Bytes p) override {
    ex.on_message(0, p, 0);
    return true;
  }
};

TEST(Gateway, AgainstTheExchangeCore_PositionsAndOpenQuantitiesAgree) {
  net::ExchangeConfig ec;
  ec.feed = samples::config();
  ec.accounts = {net::AccountConfig{"A1", "pw"}};
  ec.log_messages = 1000;
  ec.log_bytes = 1U << 16;
  ec.max_orders = 64;
  net::OuchExchange ex(ec);
  DirectTransport t(ex);
  gw::OrderGateway g(cfg());
  std::uint64_t read = 1, seq = 0;
  std::int64_t position = 0;
  std::uint64_t open_buy = 0, open_sell = 0;
  sim::Reports r;
  auto pump = [&] {
    for (; read < ex.log(0).next_sequence(); ++read) {
      r.clear();
      g.on_message(ex.log(0).at(read), r);
      for (std::size_t i = 0; i < r.size(); ++i) {
        const auto& rep = r[i];
        auto& open = rep.side == sim::Side::Buy ? open_buy : open_sell;
        open -= rep.qty;
        if (rep.kind == sim::Report::Kind::Fill) position += rep.side == sim::Side::Buy ? rep.qty : -static_cast<std::int64_t>(rep.qty);
      }
    }
  };
  auto market = [&](const tk::Buf& p) {
    md::FeedInput in;
    in.seq = ++seq;
    ASSERT_EQ(md::itch::decode(p, in.msg), md::itch::DecodeStatus::Decoded);
    ex.on_market(in);
    pump();
  };
  auto submit = [&](const lob::strategy::OrderRequest& q) {
    if (q.kind == Kind::New) (q.side == sim::Side::Buy ? open_buy : open_sell) += q.qty;
    r.clear();
    g.submit(q, t, r);
    pump();
  };
  for (const auto& m : {tk::system_event('O', 1), tk::stock_directory(samples::kLocate, "ACME", 3),
                        tk::trading_action(samples::kLocate, "ACME", 'T', 4), tk::system_event('Q', 5),
                        tk::add_order(samples::kLocate, 1, 'B', 100, "ACME", 10000, 10),
                        tk::add_order(samples::kLocate, 3, 'S', 200, "ACME", 10100, 12)}) {
    market(m);
  }
  submit(new_order(1, sim::Side::Buy, 10000, 100));
  submit(new_order(2, sim::Side::Sell, 10100, 50));
  submit(new_order(3, sim::Side::Buy, 9900, 30));
  market(tk::order_executed(samples::kLocate, 1, 60, 1, 20));  // fills 60 of order 1
  submit(cancel_order(3));
  market(tk::order_executed(samples::kLocate, 3, 200, 2, 21));  // fills all of order 2
  ASSERT_TRUE(g.replace(1, 100, 10000, t));                     // keep the remaining 40
  pump();
  submit(cancel_order(1));
  const auto p = ex.position(0);
  EXPECT_FALSE(g.inconsistent());
  EXPECT_EQ(position, p.position);
  EXPECT_EQ(position, 60 - 50);
  EXPECT_EQ(open_buy, p.open_buy);
  EXPECT_EQ(open_sell, p.open_sell);
  EXPECT_EQ(p.live_orders, 0U);
  EXPECT_EQ(g.find(1)->state, OrderState::Canceled);
  EXPECT_EQ(g.find(2)->state, OrderState::Filled);
  EXPECT_EQ(g.find(3)->state, OrderState::Canceled);
}

}  // namespace
