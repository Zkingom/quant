#pragma once

// Seeded, profile-driven command generator.
//
// A WorkloadProfile describes a command mix and an engine config chosen to
// push the system to a particular state boundary (deep books, a single price
// level, capacity flapping, near-overflow quantities, ...). The generator
// reads the CURRENT model state (a ReferenceEngine) to place prices relative
// to the spread, so the stream adapts to the book it creates; it is fully
// deterministic for (profile, seed, model state history).
//
// The "mixed" profile reproduces the M3 differential generator exactly (same
// RNG call sequence); its coverage counters are pinned by a regression test.

#include <cstdint>
#include <string>
#include <vector>

#include "lob/core/command.hpp"
#include "lob/core/config.hpp"
#include "lob/testkit/reference_engine.hpp"
#include "lob/testkit/rng.hpp"

namespace lob::testkit {

struct WorkloadProfile {
  std::string name;
  std::string purpose;  ///< the state boundary this profile targets
  EngineConfig config;

  // Phases: alternate "build" and "churn" phases of this many commands
  // (build first). 0 = churn only. A build phase behaves as churn while the
  // book is at capacity.
  long phase_length = 500;

  // Build phase, on roll in [0, 100):
  int build_cancel_below = 5;    ///< roll < this: cancel
  int build_passive_below = 92;  ///< roll < this: passive order; otherwise churn mix
  Price passive_offset_max = 8;  ///< passive price = opposite best -/+ (1 + U[0, max])
  Quantity build_qty_min = 1;
  Quantity build_qty_max = 30;

  // Churn phase, on roll in [0, 100):
  int churn_cancel_below = 25;   ///< roll < this: cancel
  int aggressive_below = 40;     ///< roll < this: aggressive order; otherwise normal
  int invalid_from = 94;         ///< roll >= this: one stateless-invalid field
  int reuse_id_percent = 10;     ///< new order reuses an older id (dead: reuse, live: duplicate)
  Price aggressive_depth_min = 3;  ///< aggressive price = anchor +/- U[min, max]
  Price aggressive_depth_max = 20;
  /// Anchor of aggressive prices: false = mid (M3), true = the opposite best
  /// (marketable just through the touch; mid if that side is empty).
  bool aggressive_from_touch = false;
  Quantity aggressive_qty_min = 30;
  Quantity aggressive_qty_max = 0;  ///< 0 = config.max_order_quantity
  Price normal_spread = 8;          ///< normal price = mid + U[-spread, spread]
  int small_qty_percent = 70;
  Quantity small_qty_max = 20;      ///< small: U[1, small_qty_max]
  Quantity large_qty_min = 21;      ///< large: U[large_qty_min, large_qty_max]
  Quantity large_qty_max = 100;

  // Cancels:
  int cancel_live_percent = 70;     ///< pick a resting id; otherwise U[0, last id + extra]
  OrderId cancel_unknown_extra = 3;

  // Side choice: P(buy) = 50 + bias in even bias phases, 50 - bias in odd
  // ones. bias 0 = unbiased (the same RNG call as Rng::side()).
  int side_bias_percent = 0;
  long bias_phase_length = 1;

  Price empty_book_mid = 30;        ///< reference price while both sides are empty
  /// true: always use empty_book_mid as the reference price (a fixed "fair
  /// value"), instead of the book's mid. Gives a thin market a restoring
  /// force; without it, it random-walks into the band edges.
  bool fixed_mid = false;
  OrderId id_space = 0;             ///< >0: fresh ids wrap within [1, id_space]

  /// Minimum run length (commands per seed) at which the profile's boundary
  /// targets are expected to be reached; shorter runs do not check targets.
  long target_commands = 0;
};

/// The v1 text format cannot express an invalid Side (DESIGN §6.3): such a
/// NewOrder is rewritten with side Buy and price 0. Both versions are
/// rejected and leave the state unchanged, so a model steered by the
/// original command stays aligned with an engine fed the rewritten one;
/// only the reject reason differs (PRICE_OUT_OF_RANGE instead of
/// INVALID_SIDE). Every other command is returned unchanged.
Command text_representable(const Command& command);

/// All standard profiles, "mixed" first.
const std::vector<WorkloadProfile>& standard_profiles();
/// nullptr if unknown.
const WorkloadProfile* find_profile(const std::string& name);

class WorkloadGenerator {
 public:
  WorkloadGenerator(const WorkloadProfile& profile, std::uint64_t seed);

  /// Next command for step `step`, given the current model state.
  Command next(long step, const ReferenceEngine& model);

  /// Highest id handed out so far + 1 (fresh-id counter).
  [[nodiscard]] OrderId next_fresh_id() const { return next_id_; }

 private:
  Command next_cancel(const ReferenceEngine& model);
  Price mid(const ReferenceEngine& model) const;
  Price passive_price(Side side, Price offset, const ReferenceEngine& model) const;
  Price clamp(Price p) const;
  Side pick_side(long step);
  OrderId take_fresh_id();

  const WorkloadProfile& profile_;
  Rng rng_;
  OrderId next_id_{1};
};

}  // namespace lob::testkit
