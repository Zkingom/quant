#pragma once

// Deterministic synthetic ITCH 5.0 stream (L3' stage 2/3/4). Multiple
// instruments, every subset message type, plus non-subset messages, all
// valid by construction: the generator keeps its own per-instrument model
// and never produces a locking or crossing add, an unknown ref, or an
// overfill. Used for pipeline equivalence tests, the zero-allocation test,
// the cachegrind CI workload and the local wall-clock benchmark.
//
// Same seed + same parameters => identical bytes on every platform
// (testkit::Rng is fully specified).

#include <cstdint>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "lob/testkit/itch_builder.hpp"
#include "lob/testkit/rng.hpp"

namespace lob::testkit::itch {

struct GeneratorParams {
  std::uint64_t seed{1};
  std::size_t messages{10000};    ///< frames to emit (directories included)
  int instruments{4};             ///< locates 1..instruments; symbols "SYM1".. ; SYM1 is the target
  int max_live_per_instrument{400};
  std::uint32_t mid{100'0000};    ///< $100.0000
  int non_subset_per_mille{30};   ///< Trade ('P') messages interleaved
};

inline std::string symbol_of(int i) { return "SYM" + std::to_string(i); }

class StreamGenerator {
 public:
  explicit StreamGenerator(const GeneratorParams& p) : p_(p), rng_(p.seed), books_(static_cast<std::size_t>(p.instruments)) {}

  Buf generate() {
    Buf out;
    append_frame(out, system_event('O', tick()));
    for (int i = 1; i <= p_.instruments; ++i) {
      append_frame(out, stock_directory(static_cast<std::uint16_t>(i), symbol_of(i), tick()));
    }
    std::size_t emitted = 1 + static_cast<std::size_t>(p_.instruments);
    while (emitted + 1 < p_.messages) {
      append_frame(out, next_message());
      ++emitted;
    }
    append_frame(out, system_event('C', tick()));
    return out;
  }

 private:
  struct Order {
    char side;
    std::uint32_t price;
    std::uint32_t shares;
  };
  struct Model {
    std::map<std::uint64_t, Order> live;
    std::map<std::uint32_t, int> bids;  // price -> order count
    std::map<std::uint32_t, int> asks;
  };

  std::uint64_t tick() {
    ts_ += static_cast<std::uint64_t>(rng_.uniform(0, 2000));  // non-decreasing
    return ts_;
  }

  static void add_level(std::map<std::uint32_t, int>& m, std::uint32_t p) { ++m[p]; }
  static void del_level(std::map<std::uint32_t, int>& m, std::uint32_t p) {
    if (--m[p] == 0) m.erase(p);
  }

  // A price strictly inside the own side of the book: never locks or crosses.
  std::uint32_t pick_price(const Model& b, char side) {
    const auto off = static_cast<std::uint32_t>(rng_.uniform(0, 40)) * 100U;
    if (side == 'B') {
      const std::uint32_t ceiling = b.asks.empty() ? p_.mid : b.asks.begin()->first;
      return ceiling > 100U + off + 1U ? ceiling - 100U - off : 1U;  // ceiling >= 2 always
    }
    const std::uint32_t floor = b.bids.empty() ? p_.mid : b.bids.rbegin()->first;
    return floor + 100U + off;
  }

  Buf next_message() {
    const int inst = static_cast<int>(rng_.uniform(1, p_.instruments));
    const auto loc = static_cast<std::uint16_t>(inst);
    Model& b = books_[static_cast<std::size_t>(inst - 1)];
    const std::uint64_t ts = tick();
    if (rng_.uniform(0, 999) < p_.non_subset_per_mille) {
      return trade_message(loc, ts);
    }
    const int op = static_cast<int>(rng_.uniform(0, 99));
    const bool must_add = b.live.empty() || (op < 45 && static_cast<int>(b.live.size()) < p_.max_live_per_instrument);
    if (must_add) {
      const char side = rng_.percent(50) ? 'B' : 'S';
      const std::uint32_t price = pick_price(b, side);
      const auto shares = static_cast<std::uint32_t>(rng_.uniform(1, 50) * 100);
      const std::uint64_t ref = next_ref_++;
      b.live[ref] = Order{side, price, shares};
      add_level(side == 'B' ? b.bids : b.asks, price);
      if (rng_.percent(20)) {
        return add_order_mpid(loc, ref, side, shares, symbol_of(inst), price, "MMID", ts);
      }
      return add_order(loc, ref, side, shares, symbol_of(inst), price, ts);
    }
    // Pick a live order deterministically (std::map iteration order).
    auto it = b.live.begin();
    std::advance(it, rng_.uniform(0, static_cast<std::int64_t>(b.live.size()) - 1));
    const std::uint64_t ref = it->first;
    Order& o = it->second;
    auto& levels = o.side == 'B' ? b.bids : b.asks;
    if (op < 65) {  // execute (partial or full)
      const auto qty = static_cast<std::uint32_t>(rng_.uniform(1, o.shares));
      const std::uint64_t match = next_match_++;
      Buf m = rng_.percent(25) ? order_executed_price(loc, ref, qty, match, 'Y', o.price, ts)
                               : order_executed(loc, ref, qty, match, ts);
      o.shares -= qty;
      if (o.shares == 0) {
        del_level(levels, o.price);
        b.live.erase(it);
      }
      return m;
    }
    if (op < 78 && o.shares > 1) {  // partial cancel
      const auto qty = static_cast<std::uint32_t>(rng_.uniform(1, o.shares - 1));
      o.shares -= qty;
      return order_cancel(loc, ref, qty, ts);
    }
    if (op < 90) {  // delete
      del_level(levels, o.price);
      b.live.erase(it);
      return order_delete(loc, ref, ts);
    }
    // replace: same side, new ref, new price on the own side
    const char side = o.side;
    del_level(levels, o.price);
    b.live.erase(it);
    const std::uint32_t price = pick_price(b, side);
    const auto shares = static_cast<std::uint32_t>(rng_.uniform(1, 50) * 100);
    const std::uint64_t nref = next_ref_++;
    b.live[nref] = Order{side, price, shares};
    add_level(side == 'B' ? b.bids : b.asks, price);
    return order_replace(loc, ref, nref, shares, price, ts);
  }

  GeneratorParams p_;
  Rng rng_;
  std::vector<Model> books_;
  std::uint64_t ts_{34'200'000'000'000ULL};  // 09:30:00
  std::uint64_t next_ref_{1};
  std::uint64_t next_match_{1};
};

inline Buf generate_stream(const GeneratorParams& p) { return StreamGenerator{p}.generate(); }

}  // namespace lob::testkit::itch
