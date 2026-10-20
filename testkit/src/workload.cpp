#include "lob/testkit/workload.hpp"

#include <algorithm>
#include <limits>

#include "lob/core/checked.hpp"

namespace lob::testkit {

namespace {

constexpr std::int64_t kI64Max = std::numeric_limits<std::int64_t>::max();

/// Saturating a + b: the generator works near the int64 limits in the
/// extreme_price profile and must not overflow itself.
Price add_sat(Price a, Price b) {
  if (const auto sum = checked_add(a, b)) {
    return *sum;
  }
  return b > 0 ? std::numeric_limits<Price>::max() : std::numeric_limits<Price>::min();
}

/// value + 1, or 0 (also invalid) if that would overflow.
std::int64_t one_past(std::int64_t value) { return checked_add<std::int64_t>(value, 1).value_or(0); }

std::vector<WorkloadProfile> make_profiles() {
  std::vector<WorkloadProfile> profiles;

  // ---- mixed: the M3 differential workload, unchanged (regression anchor).
  {
    WorkloadProfile p;
    p.name = "mixed";
    p.purpose = "M3 baseline mix: build/churn phases, sweeps, invalid input, capacity 40";
    p.config = {.min_price = 1, .max_price = 60, .max_order_quantity = 100, .max_live_orders = 40};
    p.target_commands = 5'000;
    profiles.push_back(p);
  }
  // ---- deep_book: thousands of resting orders over a wide band; sweeps
  // that cross many levels in one command.
  {
    WorkloadProfile p;
    p.name = "deep_book";
    p.purpose = "deep book: up to 5000 resting orders over >1000 levels; multi-level sweeps";
    p.config = {.min_price = 1, .max_price = 4'000, .max_order_quantity = 5'000,
                .max_live_orders = 5'000};
    p.phase_length = 6'000;
    p.passive_offset_max = 1'800;  // spreads resting orders over > 1000 levels per side
    p.build_qty_max = 200;
    p.churn_cancel_below = 25;
    p.aggressive_below = 35;
    p.aggressive_depth_min = 20;
    p.aggressive_depth_max = 1'800;
    p.aggressive_qty_min = 3'000;  // ~30+ makers of ~100 lots per sweep
    p.normal_spread = 300;
    p.small_qty_max = 50;
    p.large_qty_min = 51;
    p.large_qty_max = 500;
    p.empty_book_mid = 2'000;
    p.target_commands = 30'000;
    profiles.push_back(p);
  }
  // ---- single_price: the whole band is one price. One side at a time,
  // long FIFO queues, every trade at an equal price.
  {
    WorkloadProfile p;
    p.name = "single_price";
    p.purpose = "band of one price (min == max): long FIFO queues, equal-price trades only";
    p.config = {.min_price = 100, .max_price = 100, .max_order_quantity = 200,
                .max_live_orders = 400};
    p.phase_length = 0;
    p.churn_cancel_below = 20;
    p.aggressive_below = 30;
    p.aggressive_qty_min = 150;  // vs. resting orders of 1..5 lots: >= 30 makers per sweep
    p.small_qty_percent = 90;
    p.small_qty_max = 5;
    p.large_qty_min = 6;
    p.large_qty_max = 20;
    // With one price, any opposite order trades at once, so an unbiased
    // stream only grows a queue like a random walk (~sqrt n). Alternate an
    // 80/20 side bias to build long queues and then sweep them.
    p.side_bias_percent = 30;
    p.bias_phase_length = 1'500;
    p.empty_book_mid = 100;
    p.target_commands = 20'000;
    profiles.push_back(p);
  }
  // ---- high_density: most orders trade on arrival; large resting orders
  // serve several small takers, so maker partial-fill chains dominate.
  {
    WorkloadProfile p;
    p.name = "high_density";
    p.purpose = "most orders trade on arrival; large makers partially filled by many small takers";
    p.config = {.min_price = 1, .max_price = 60, .max_order_quantity = 100, .max_live_orders = 200};
    p.phase_length = 0;
    p.churn_cancel_below = 10;
    p.aggressive_below = 70;  // 60% aggressive, 24% normal (replenishing both sides)
    // Large resting orders, small aggressive ones: a maker serves several
    // takers. In steady state every resting order is liquidity some later
    // order consumes; if each maker served only one taker, the share of
    // orders trading on arrival would settle near 50% (M4 analysis).
    p.small_qty_percent = 30;
    p.small_qty_max = 5;
    p.large_qty_min = 20;
    p.large_qty_max = 60;
    p.aggressive_qty_min = 1;
    p.aggressive_qty_max = 8;
    // Priced just through the touch, around a fixed reference price: anchored
    // at the book's mid, each remainder would rest away from the touch and
    // walk the market into the band edges (observed: 84% of trades at a band
    // edge in the first calibration).
    p.aggressive_from_touch = true;
    p.aggressive_depth_min = 0;
    p.aggressive_depth_max = 2;
    p.fixed_mid = true;
    p.target_commands = 20'000;
    profiles.push_back(p);
  }
  // ---- thin_book: small resting orders, moderately sized marketable ones:
  // the opposite side is traded empty often (empty-side transitions).
  {
    WorkloadProfile p;
    p.name = "thin_book";
    p.purpose = "thin book: a side is traded empty in >= 5% of commands";
    p.config = {.min_price = 1, .max_price = 60, .max_order_quantity = 100, .max_live_orders = 200};
    p.phase_length = 0;
    p.churn_cancel_below = 10;
    p.aggressive_below = 85;  // 75% aggressive, 9% normal
    p.small_qty_percent = 90;
    p.small_qty_max = 3;
    p.large_qty_min = 4;
    p.large_qty_max = 10;
    p.aggressive_qty_min = 4;
    p.aggressive_qty_max = 12;
    p.aggressive_from_touch = true;
    p.aggressive_depth_min = 0;
    p.aggressive_depth_max = 2;
    p.fixed_mid = true;
    p.target_commands = 20'000;
    profiles.push_back(p);
  }
  // ---- cancel_heavy: cancels dominate, many of unknown/dead ids.
  {
    WorkloadProfile p;
    p.name = "cancel_heavy";
    p.purpose = "70% cancels, half for ids that are not resting; heavy index churn";
    p.config = {.min_price = 1, .max_price = 60, .max_order_quantity = 100, .max_live_orders = 100};
    p.churn_cancel_below = 70;
    p.aggressive_below = 75;
    p.invalid_from = 97;
    p.cancel_live_percent = 50;
    p.cancel_unknown_extra = 50;
    p.target_commands = 20'000;
    profiles.push_back(p);
  }
  // ---- id_churn: a tiny id space, so "fresh" ids collide with live ones
  // and dead ids are reused constantly.
  {
    WorkloadProfile p;
    p.name = "id_churn";
    p.purpose = "ids wrap within [1, 64]: constant duplicate rejections and dead-id reuse";
    p.config = {.min_price = 1, .max_price = 60, .max_order_quantity = 100, .max_live_orders = 40};
    p.id_space = 64;
    p.reuse_id_percent = 20;
    p.target_commands = 20'000;
    profiles.push_back(p);
  }
  // ---- capacity_edge: capacity 4; the book sits on the BOOK_FULL boundary.
  {
    WorkloadProfile p;
    p.name = "capacity_edge";
    p.purpose = "capacity 4: constant BOOK_FULL decisions, fills that free slots";
    p.config = {.min_price = 1, .max_price = 60, .max_order_quantity = 100, .max_live_orders = 4};
    p.phase_length = 50;
    p.target_commands = 20'000;
    profiles.push_back(p);
  }
  // ---- extreme_quantity: per-order quantities near INT64_MAX / 64, so a
  // full level's total is near INT64_MAX and cumulative totals exceed it.
  {
    WorkloadProfile p;
    p.name = "extreme_quantity";
    p.purpose = "quantities near INT64_MAX/64: level totals near INT64_MAX (D-006 bound)";
    const Quantity max_qty = kI64Max / 64;
    // Two prices: one level per side, so up to 64 near-maximal orders share
    // a level and its total approaches 64 * max_qty <= INT64_MAX.
    p.config = {.min_price = 1, .max_price = 2, .max_order_quantity = max_qty,
                .max_live_orders = 64};
    p.passive_offset_max = 0;
    p.aggressive_depth_min = 1;
    p.aggressive_depth_max = 1;
    p.normal_spread = 1;
    p.build_qty_min = max_qty / 2;
    p.build_qty_max = max_qty;
    p.aggressive_qty_min = max_qty / 2;
    p.small_qty_percent = 20;
    p.small_qty_max = max_qty / 4;
    p.large_qty_min = max_qty / 2;
    p.large_qty_max = max_qty;
    p.empty_book_mid = 1;
    p.target_commands = 20'000;
    profiles.push_back(p);
  }
  // ---- extreme_price: the price band sits next to the int64 limit
  // (max_price * max_order_quantity <= INT64_MAX holds exactly).
  {
    WorkloadProfile p;
    p.name = "extreme_price";
    p.purpose = "band [INT64_MAX/1000 - 60, INT64_MAX/1000]: prices near the representable limit";
    const Price top = kI64Max / 1'000;
    p.config = {.min_price = top - 60, .max_price = top, .max_order_quantity = 1'000,
                .max_live_orders = 40};
    p.empty_book_mid = top - 30;
    p.aggressive_qty_max = 100;
    p.target_commands = 20'000;
    profiles.push_back(p);
  }
  return profiles;
}

}  // namespace

const std::vector<WorkloadProfile>& standard_profiles() {
  static const std::vector<WorkloadProfile> profiles = make_profiles();
  return profiles;
}

const WorkloadProfile* find_profile(const std::string& name) {
  for (const WorkloadProfile& p : standard_profiles()) {
    if (p.name == name) {
      return &p;
    }
  }
  return nullptr;
}

// ------------------------------------------------------------------ generator

WorkloadGenerator::WorkloadGenerator(const WorkloadProfile& profile, std::uint64_t seed)
    : profile_(profile), rng_(seed) {}

OrderId WorkloadGenerator::take_fresh_id() {
  const OrderId id = next_id_++;
  if (profile_.id_space > 0 && next_id_ > profile_.id_space) {
    next_id_ = 1;
  }
  return id;
}

Side WorkloadGenerator::pick_side(long step) {
  const WorkloadProfile& p = profile_;
  const int buy_percent =
      p.side_bias_percent == 0
          ? 50
          : ((step / p.bias_phase_length) % 2 == 0 ? 50 + p.side_bias_percent
                                                   : 50 - p.side_bias_percent);
  return rng_.percent(buy_percent) ? Side::Buy : Side::Sell;  // == Rng::side() when unbiased
}

Price WorkloadGenerator::clamp(Price p) const {
  return std::clamp<Price>(p, profile_.config.min_price, profile_.config.max_price);
}

Price WorkloadGenerator::mid(const ReferenceEngine& model) const {
  if (profile_.fixed_mid) {
    return profile_.empty_book_mid;
  }
  const auto bid = model.best_price(Side::Buy);
  const auto ask = model.best_price(Side::Sell);
  if (bid && ask) {
    return *bid + (*ask - *bid) / 2;  // == (bid + ask) / 2 without overflow (bid < ask)
  }
  if (bid) {
    return add_sat(*bid, 1);
  }
  if (ask) {
    return add_sat(*ask, -1);
  }
  return profile_.empty_book_mid;
}

Price WorkloadGenerator::passive_price(Side side, Price offset, const ReferenceEngine& model) const {
  const Price gap = 1 + offset;
  const auto best = model.best_price(opposite(side));
  const Price anchor = best ? *best : mid(model);
  return clamp(side == Side::Buy ? add_sat(anchor, -gap) : add_sat(anchor, gap));
}

Command WorkloadGenerator::next_cancel(const ReferenceEngine& model) {
  const auto& orders = model.orders();
  if (!orders.empty() && rng_.percent(profile_.cancel_live_percent)) {
    const auto pick = rng_.uniform(0, static_cast<std::int64_t>(orders.size()) - 1);
    return CancelOrder{.id = orders[static_cast<std::size_t>(pick)].id};
  }
  const auto hi = static_cast<std::int64_t>(next_id_) + static_cast<std::int64_t>(profile_.cancel_unknown_extra);
  return CancelOrder{.id = static_cast<OrderId>(rng_.uniform(0, hi))};
}

// The structure and RNG call order below are those of the M3 differential
// generator; the "mixed" profile's parameters are its constants.
Command WorkloadGenerator::next(long step, const ReferenceEngine& model) {
  const WorkloadProfile& p = profile_;
  const bool building =
      p.phase_length > 0 && (step / p.phase_length) % 2 == 0 &&
      static_cast<std::int64_t>(model.order_count()) < p.config.max_live_orders;

  const int roll = static_cast<int>(rng_.uniform(0, 99));
  if (building) {
    if (roll < p.build_cancel_below) {
      return next_cancel(model);
    }
    if (roll < p.build_passive_below) {
      const Price offset = rng_.uniform(0, p.passive_offset_max);
      const Side side = pick_side(step);
      const OrderId id = take_fresh_id();
      const Price price = passive_price(side, offset, model);
      return NewOrder{.id = id,
                      .side = side,
                      .price = price,
                      .quantity = rng_.uniform(p.build_qty_min, p.build_qty_max)};
    }
    // otherwise: fall through to the churn mix
  }
  if (!building && roll < p.churn_cancel_below) {
    return next_cancel(model);
  }

  NewOrder order{.id = 0, .side = pick_side(step), .price = 0, .quantity = 0};
  order.id = (next_id_ > 1 && rng_.percent(p.reuse_id_percent))
                 ? static_cast<OrderId>(rng_.uniform(1, static_cast<std::int64_t>(next_id_) - 1))
                 : take_fresh_id();
  const Price m = mid(model);
  if (roll < p.aggressive_below) {
    const Price depth = rng_.uniform(p.aggressive_depth_min, p.aggressive_depth_max);
    Price anchor = m;
    if (p.aggressive_from_touch) {
      anchor = model.best_price(opposite(order.side)).value_or(m);
    }
    order.price = clamp(order.side == Side::Buy ? add_sat(anchor, depth) : add_sat(anchor, -depth));
    const Quantity max_qty =
        p.aggressive_qty_max > 0 ? p.aggressive_qty_max : p.config.max_order_quantity;
    order.quantity = rng_.uniform(p.aggressive_qty_min, max_qty);
  } else {
    order.price = clamp(add_sat(m, rng_.uniform(-p.normal_spread, p.normal_spread)));
    order.quantity = rng_.percent(p.small_qty_percent) ? rng_.uniform(1, p.small_qty_max)
                                                       : rng_.uniform(p.large_qty_min, p.large_qty_max);
  }
  if (roll >= p.invalid_from) {
    switch (rng_.uniform(0, 4)) {
      case 0:
        order.id = 0;
        break;
      case 1:
        order.side = static_cast<Side>(rng_.uniform(2, 255));
        break;
      case 2:
        order.price = rng_.percent(50) ? 0 : one_past(p.config.max_price);
        break;
      case 3:
        order.quantity = rng_.percent(50) ? 0 : -rng_.uniform(1, 5);
        break;
      default:
        order.quantity = one_past(p.config.max_order_quantity);
        break;
    }
  }
  return order;
}

}  // namespace lob::testkit
