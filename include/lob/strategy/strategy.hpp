#pragma once

// Strategy interface (L3' stage 3). DESIGN §7.6.
//
// Three callbacks, all driven by market-data time (ITCH timestamps), never
// by the wall clock, so that a strategy behaves identically in file replay
// and in the multi-threaded pipeline:
//   on_book_update(top, out)  after every published book change (and when
//                             the book stops being usable: top.live = false)
//   on_fill(fill, out)        execution report for one of our orders: a fill
//                             (qty > 0), or qty == 0 with leaves == 0 when
//                             the order ended unfilled (risk reject, exchange
//                             reject, or a system cancel such as the kill switch)
//   on_timer(ts, out)         when ITCH time crosses the next timer boundary
// A strategy only *requests* orders by appending to `out` (fixed capacity,
// no allocation); pre-trade risk decides what reaches the exchange.
//
// Dispatch: the hot path uses static dispatch (CRTP: StrategyBase<Derived>
// forwards with static_cast, so the call inlines). VirtualStrategy is the
// same interface with virtual functions, kept only to measure the
// difference (bench/itch_bench.cpp, MODULE_NOTES/L3.md).

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "lob/md/itch.hpp"

namespace lob::strategy {

using md::itch::Side;

struct BookTop {
  std::uint64_t seq{0};
  std::uint64_t ts{0};
  std::uint32_t bid_price{0};  ///< 0 = no bid
  std::uint32_t ask_price{0};  ///< 0 = no ask
  std::uint64_t bid_shares{0};
  std::uint64_t ask_shares{0};
  bool live{false};            ///< false: the book must not be used (feed not Live)
};

struct Fill {
  std::uint64_t client_id{0};
  Side side{Side::Buy};
  std::uint32_t price{0};
  std::uint32_t qty{0};
  std::uint32_t leaves{0};  ///< remaining open quantity of the order after this fill
  std::uint64_t ts{0};
};

struct OrderRequest {
  enum class Kind : std::uint8_t { New, Cancel };
  Kind kind{Kind::New};
  std::uint64_t client_id{0};
  Side side{Side::Buy};
  std::uint32_t price{0};
  std::uint32_t qty{0};
};

/// Fixed-capacity output buffer: appending beyond capacity is dropped and
/// counted (a strategy bug, surfaced in statistics), never an allocation.
class Actions {
 public:
  static constexpr std::size_t kCapacity = 16;
  void push(const OrderRequest& r) noexcept {
    if (n_ < kCapacity) {
      a_[n_++] = r;
    } else {
      ++overflow_;
    }
  }
  void clear() noexcept { n_ = 0; }
  [[nodiscard]] std::size_t size() const noexcept { return n_; }
  [[nodiscard]] const OrderRequest& operator[](std::size_t i) const noexcept { return a_[i]; }
  [[nodiscard]] std::uint64_t overflow() const noexcept { return overflow_; }

 private:
  std::array<OrderRequest, kCapacity> a_{};
  std::size_t n_{0};
  std::uint64_t overflow_{0};
};

/// CRTP base: Derived implements on_book_update / on_fill / on_timer.
template <class Derived>
class StrategyBase {
 public:
  void book_update(const BookTop& t, Actions& out) { self().on_book_update(t, out); }
  void fill(const Fill& f, Actions& out) { self().on_fill(f, out); }
  void timer(std::uint64_t ts, Actions& out) { self().on_timer(ts, out); }

 protected:
  StrategyBase() = default;

 private:
  Derived& self() noexcept { return static_cast<Derived&>(*this); }
};

/// Virtual-dispatch interface with the same three callbacks (comparison only).
class VirtualStrategy {
 public:
  virtual ~VirtualStrategy() = default;
  virtual void book_update(const BookTop& t, Actions& out) = 0;
  virtual void fill(const Fill& f, Actions& out) = 0;
  virtual void timer(std::uint64_t ts, Actions& out) = 0;

 protected:
  VirtualStrategy() = default;
  VirtualStrategy(const VirtualStrategy&) = default;
  VirtualStrategy& operator=(const VirtualStrategy&) = default;
};

// ---------------------------------------------------------------- example

/// Market maker with inventory skew. Quotes one bid and one ask around the
/// reservation price  r = mid - skew * inventory  (prices in ITCH units,
/// 1/10000 $; integer arithmetic only):
///   bid = r - half_spread, ask = r + half_spread, both rounded to `tick`
///   and kept passive (bid < best ask, ask > best bid).
/// A side stops quoting once |inventory| reaches max_inventory in that
/// direction. When the book is not live it cancels everything and waits.
/// It requotes only when the desired price differs from the resting one.
struct MarketMakerParams {
  std::uint32_t tick{100};          ///< $0.01
  std::uint32_t half_spread{200};   ///< $0.02
  std::uint32_t quote_size{100};
  std::int64_t skew_per_share{1};   ///< price units per share of inventory
  std::int64_t max_inventory{1000};
};

class MarketMaker : public StrategyBase<MarketMaker> {
 public:
  explicit MarketMaker(const MarketMakerParams& p = {}) : p_(p) {}

  void on_book_update(const BookTop& t, Actions& out);
  void on_fill(const Fill& f, Actions& out);
  void on_timer(std::uint64_t ts, Actions& out);

  [[nodiscard]] std::int64_t inventory() const noexcept { return inventory_; }
  [[nodiscard]] std::uint64_t timers() const noexcept { return timers_; }

 private:
  struct Quote {
    std::uint64_t id{0};  ///< 0 = none resting
    std::uint32_t price{0};
  };
  void requote(Side side, std::uint32_t price, bool allowed, Actions& out);
  void cancel_all(Actions& out);

  MarketMakerParams p_;
  std::int64_t inventory_{0};
  Quote bid_{};
  Quote ask_{};
  std::uint64_t next_id_{1};
  std::uint64_t timers_{0};
};

/// The same strategy behind the virtual interface; created out of line so
/// the compiler cannot devirtualise the comparison.
[[nodiscard]] std::unique_ptr<VirtualStrategy> make_virtual_market_maker(const MarketMakerParams& p);

}  // namespace lob::strategy
