#pragma once

// Trading stage (L3' stage 3): strategy + OMS + pre-trade risk + simulated
// exchange, run as the downstream of the pipeline's BookStage -- i.e. on the
// book thread, synchronously. DESIGN §7.9.
//
// Per published book update, in this order:
//   1. the market event goes to the simulated exchange (fills of our resting
//      orders caused by what the market just did) -> reports -> OMS -> strategy.on_fill
//   2. strategy.on_book_update(top) -> requests -> risk -> exchange -> reports
//   3. timer: when ITCH time crosses the next boundary, strategy.on_timer
// When the feed leaves Live: the kill switch trips (StaleBook), every open
// order is cancelled, and the strategy sees top.live = false.
//
// Why synchronous on the book thread: a separate exchange thread would make
// the arrival of fills relative to market data depend on scheduling, and the
// "replay == pipeline" gate would no longer hold. Everything here is a pure
// function of the input stream.
//
// Digests: orders (every request and its risk verdict), fills (every
// report), position (final position, cash, open quantity) -- compared
// between file replay and the multi-threaded pipeline.

#include <cstdint>
#include <span>

#include "lob/md/feed.hpp"
#include "lob/pipeline/pipeline.hpp"
#include "lob/risk/risk.hpp"
#include "lob/sim/sim_exchange.hpp"
#include "lob/strategy/strategy.hpp"

namespace lob::sim {

struct TradingConfig {
  SimConfig sim{};
  risk::RiskLimits risk{};
  std::uint64_t timer_interval_ns{1'000'000'000ULL};  ///< ITCH time
};

struct TradingDigest {
  std::uint64_t orders{md::kHashSeed};
  std::uint64_t fills{md::kHashSeed};
  std::uint64_t n_requests{0};
  std::uint64_t n_accepted{0};
  std::uint64_t n_rejected{0};
  std::uint64_t n_fills{0};
  std::uint64_t filled_qty{0};
};

namespace detail {
inline void mix(std::uint64_t& h, std::uint64_t v, int bytes) noexcept {
  for (int i = 0; i < bytes; ++i) {
    h ^= (v >> (8 * i)) & 0xffU;
    h *= 0x100000001b3ULL;
  }
}
}  // namespace detail

/// `S` provides book_update / fill / timer (StrategyBase<Derived> or a
/// VirtualStrategy reference wrapper).
template <class S>
class TradingStage {
 public:
  TradingStage(const TradingConfig& cfg, S& strat) : cfg_(cfg), strat_(strat), risk_(cfg.risk), ex_(cfg.sim) {}

  void on_feed(const md::FeedInput&, std::span<const md::FeedEvent> events, const md::FeedHandler& feed,
               std::uint64_t, pipeline::LatencyRecorder* rec, bool sampled) {
    t_strategy_ = t_risk_ = t_engine_ = 0;
    for (const md::FeedEvent& e : events) {
      handle(e, feed);
    }
    if (sampled && rec != nullptr) {
      if (t_strategy_ != 0) rec->record(pipeline::kStrategy, t_strategy_);
      if (t_risk_ != 0) rec->record(pipeline::kRisk, t_risk_);
      if (t_engine_ != 0) rec->record(pipeline::kEngine, t_engine_);
    }
  }

  [[nodiscard]] const TradingDigest& digest() const noexcept { return d_; }
  [[nodiscard]] std::int64_t position() const noexcept { return ex_pos_.position; }
  [[nodiscard]] std::int64_t cash() const noexcept { return cash_; }
  [[nodiscard]] const risk::Exposure& exposure() const noexcept { return ex_pos_; }
  [[nodiscard]] const risk::RiskGate& risk() const noexcept { return risk_; }
  [[nodiscard]] risk::RiskGate& risk() noexcept { return risk_; }
  [[nodiscard]] const SimExchange& exchange() const noexcept { return ex_; }
  [[nodiscard]] std::uint32_t last_mid() const noexcept { return last_mid_; }

  /// FNV over the final position, cash and open quantity.
  [[nodiscard]] std::uint64_t position_hash() const noexcept {
    std::uint64_t h = md::kHashSeed;
    detail::mix(h, static_cast<std::uint64_t>(ex_pos_.position), 8);
    detail::mix(h, static_cast<std::uint64_t>(cash_), 8);
    detail::mix(h, ex_pos_.open_buy, 8);
    detail::mix(h, ex_pos_.open_sell, 8);
    return h;
  }

  /// Mark-to-market P&L in price units x shares (1e-4 $), at the last mid.
  [[nodiscard]] std::int64_t pnl() const noexcept {
    return cash_ + ex_pos_.position * static_cast<std::int64_t>(last_mid_);
  }

 private:
  void handle(const md::FeedEvent& e, const md::FeedHandler& feed) {
    if (e.timestamp > now_) now_ = e.timestamp;
    if (e.kind == md::FeedEvent::Kind::BookUpdate) {
      // 1. what the market did to our resting orders
      reports_.clear();
      const std::uint64_t a = pipeline::ticks();
      ex_.on_market(e, reports_);
      t_engine_ += pipeline::ticks() - a;
      deliver(feed);
      // 2. the strategy reacts to the new top of book
      top_ = strategy::BookTop{e.seq, e.timestamp, e.bid_price, e.ask_price, e.bid_shares, e.ask_shares, true};
      if (e.bid_price != 0 && e.ask_price != 0) {
        last_mid_ = static_cast<std::uint32_t>((static_cast<std::uint64_t>(e.bid_price) + e.ask_price) / 2);
      }
      call([&](strategy::Actions& out) { strat_.book_update(top_, out); }, feed);
    } else if (e.state != md::FeedState::Live && top_.live) {
      // The book became unusable: kill switch, cancel everything, tell the strategy.
      risk_.trip(risk::TripReason::StaleBook);
      top_.live = false;
      cancel_all(feed);
      call([&](strategy::Actions& out) { strat_.book_update(top_, out); }, feed);
    }
    // 3. timers by market time
    if (next_timer_ == 0) {
      next_timer_ = (now_ / cfg_.timer_interval_ns + 1) * cfg_.timer_interval_ns;
    }
    while (now_ >= next_timer_) {
      const std::uint64_t ts = next_timer_;
      next_timer_ += cfg_.timer_interval_ns;
      call([&](strategy::Actions& out) { strat_.timer(ts, out); }, feed);
    }
  }

  template <class F>
  void call(F&& f, const md::FeedHandler& feed) {
    actions_.clear();
    const std::uint64_t a = pipeline::ticks();
    f(actions_);
    t_strategy_ += pipeline::ticks() - a;
    process(feed);
  }

  void process(const md::FeedHandler& feed) {
    // Copy: delivering reports may call the strategy, which reuses actions_.
    const strategy::Actions batch = actions_;
    for (std::size_t i = 0; i < batch.size(); ++i) {
      const strategy::OrderRequest& r = batch[i];
      ++d_.n_requests;
      detail::mix(d_.orders, static_cast<std::uint64_t>(r.kind), 1);
      detail::mix(d_.orders, r.client_id, 8);
      reports_.clear();
      if (r.kind == strategy::OrderRequest::Kind::Cancel) {
        const std::uint64_t a = pipeline::ticks();
        ex_.submit(r, feed.book(), reports_);
        t_engine_ += pipeline::ticks() - a;
      } else {
        detail::mix(d_.orders, static_cast<std::uint64_t>(r.side), 1);
        detail::mix(d_.orders, r.price, 4);
        detail::mix(d_.orders, r.qty, 4);
        const risk::Reference ref{top_.bid_price, top_.ask_price, top_.live};
        const std::uint64_t a = pipeline::ticks();
        const risk::Verdict v = risk_.check_new(r.side, r.price, r.qty, ex_pos_, ref, now_);
        t_risk_ += pipeline::ticks() - a;
        detail::mix(d_.orders, static_cast<std::uint64_t>(v), 1);
        if (v != risk::Verdict::Accept) {
          ++d_.n_rejected;
          reports_.push(Report{Report::Kind::Rejected, r.client_id, r.side, r.price, 0, 0});
          if (risk_.tripped()) cancel_all_into(feed, reports_);
        } else {
          ++d_.n_accepted;
          (r.side == Side::Buy ? ex_pos_.open_buy : ex_pos_.open_sell) += r.qty;
          const std::uint64_t b = pipeline::ticks();
          ex_.submit(r, feed.book(), reports_);
          t_engine_ += pipeline::ticks() - b;
        }
      }
      deliver(feed);
    }
  }

  void cancel_all_into(const md::FeedHandler& feed, Reports& out) {
    for (const OpenOrder& o : ex_.open_orders()) {
      if (o.id != 0) {
        ex_.submit(strategy::OrderRequest{strategy::OrderRequest::Kind::Cancel, o.id, o.side, 0, 0}, feed.book(), out);
      }
    }
  }

  void cancel_all(const md::FeedHandler& feed) {
    reports_.clear();
    cancel_all_into(feed, reports_);
    deliver(feed);
  }

  /// OMS update for every pending report, then the strategy's on_fill.
  void deliver(const md::FeedHandler& feed) {
    if (reports_.size() == 0) return;
    const Reports batch = reports_;
    reports_.clear();
    for (std::size_t i = 0; i < batch.size(); ++i) {
      const Report& r = batch[i];
      detail::mix(d_.fills, static_cast<std::uint64_t>(r.kind), 1);
      detail::mix(d_.fills, r.client_id, 8);
      detail::mix(d_.fills, r.price, 4);
      detail::mix(d_.fills, r.qty, 4);
      detail::mix(d_.fills, r.leaves, 4);
      std::uint64_t& open = r.side == Side::Buy ? ex_pos_.open_buy : ex_pos_.open_sell;
      if (r.kind == Report::Kind::Fill) {
        ++d_.n_fills;
        d_.filled_qty += r.qty;
        const auto q = static_cast<std::int64_t>(r.qty);
        ex_pos_.position += r.side == Side::Buy ? q : -q;
        cash_ += (r.side == Side::Buy ? -q : q) * static_cast<std::int64_t>(r.price);
        open -= std::min<std::uint64_t>(open, r.qty);
      } else {
        open -= std::min<std::uint64_t>(open, r.qty);  // unfilled remainder leaves the book
      }
      const strategy::Fill f{r.client_id, r.side, r.price, r.kind == Report::Kind::Fill ? r.qty : 0U, r.leaves, now_};
      call([&](strategy::Actions& out) { strat_.fill(f, out); }, feed);
    }
  }

  TradingConfig cfg_;
  S& strat_;
  risk::RiskGate risk_;
  SimExchange ex_;
  risk::Exposure ex_pos_{};
  std::int64_t cash_{0};
  std::uint32_t last_mid_{0};
  strategy::BookTop top_{};
  strategy::Actions actions_{};
  Reports reports_{};
  TradingDigest d_{};
  std::uint64_t now_{0};
  std::uint64_t next_timer_{0};
  std::uint64_t t_strategy_{0}, t_risk_{0}, t_engine_{0};
};

/// Runs two trading stages (optimistic and conservative fill model) side by
/// side over the same market data: the two results bound reality.
template <class A, class B>
struct Both {
  A& a;
  B& b;
  void on_feed(const md::FeedInput& in, std::span<const md::FeedEvent> ev, const md::FeedHandler& f,
               std::uint64_t t, pipeline::LatencyRecorder* rec, bool s) {
    a.on_feed(in, ev, f, t, rec, s);
    b.on_feed(in, ev, f, t, nullptr, false);
  }
};

}  // namespace lob::sim
