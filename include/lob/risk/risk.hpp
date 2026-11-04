#pragma once

// Pre-trade risk (L3' stage 3). DESIGN §7.7.
//
// Every NEW order request passes, in this fixed order (the first failure is
// the reported reason; the order is observable and tested):
//   1. kill switch      -- tripped: reject every new order
//   1b. trading phase   -- the instrument must be tradable (regular hours,
//                          state 'T', book not locked/crossed; D-054).
//                          Expected during halts: NOT counted toward the
//                          reject storm and does not trip the kill switch.
//   2. reference price  -- the book must be live and two-sided (the band and
//                          the notional need a reference)
//   3. fat finger: qty  -- 0 < qty <= max_order_qty
//   4. fat finger: notional -- price * qty <= max_notional
//   5. price band       -- |price - mid| * 10000 <= band_bps * mid
//   6. max position     -- worst case if every open order on that side and
//                          this one filled: |position +/- open +/- qty| <= max_position
//   7. order rate       -- token bucket refilled by ITCH time; costs 1 token
// Cancels are never blocked (not even by the kill switch): reducing risk
// must always be possible. Time is the market-data timestamp, never the wall
// clock, so the same input gives the same decisions in every mode.
//
// Kill switch semantics: trip(reason) is sticky -- it stays tripped until an
// explicit reset() (an operator action), even if the condition clears. It
// trips automatically when the book stops being live (StaleBook, raised by
// the trading stage) and when `max_rejects_in_window` risk rejections occur
// within `reject_window_ns` (RejectStorm). On a trip the trading stage
// cancels every open order.

#include <cstdint>
#include <string_view>

#include "lob/md/itch.hpp"

namespace lob::risk {

using md::itch::Side;

struct RiskLimits {
  std::int64_t max_position{2000};
  std::uint32_t max_order_qty{1000};
  std::uint64_t max_notional{10'000'000'000ULL};  ///< price units (1e-4 $) x shares: $1 000 000
  std::uint32_t band_bps{100};                                     ///< 1%
  std::uint64_t bucket_capacity{20};                               ///< burst, in orders
  std::uint64_t refill_per_second{50};                             ///< sustained orders per (ITCH) second
  std::uint32_t max_rejects_in_window{50};
  std::uint64_t reject_window_ns{1'000'000'000ULL};
};

enum class Verdict : std::uint8_t {
  Accept,
  KillSwitch,
  NotTrading,
  NoReference,
  FatFingerQty,
  FatFingerNotional,
  PriceBand,
  MaxPosition,
  RateLimit,
};
enum class TripReason : std::uint8_t { None, Manual, StaleBook, RejectStorm };

[[nodiscard]] std::string_view to_string(Verdict v) noexcept;
[[nodiscard]] std::string_view to_string(TripReason r) noexcept;

/// What the gate needs to know about the account (maintained by the OMS).
struct Exposure {
  std::int64_t position{0};
  std::uint64_t open_buy{0};   ///< open (unfilled) buy quantity
  std::uint64_t open_sell{0};
};

struct Reference {
  std::uint32_t bid{0};
  std::uint32_t ask{0};
  bool live{false};
  bool tradable{false};  ///< md::tradable() of the latest event
};

class RiskGate {
 public:
  explicit RiskGate(const RiskLimits& limits);

  /// Checks one new order at market time `ts`. On Accept the token is spent.
  [[nodiscard]] Verdict check_new(Side side, std::uint32_t price, std::uint32_t qty, const Exposure& ex,
                                  const Reference& ref, std::uint64_t ts) noexcept;

  void trip(TripReason why) noexcept {
    if (trip_ == TripReason::None) trip_ = why;
  }
  void reset() noexcept;
  [[nodiscard]] bool tripped() const noexcept { return trip_ != TripReason::None; }
  [[nodiscard]] TripReason trip_reason() const noexcept { return trip_; }

  [[nodiscard]] std::uint64_t accepted() const noexcept { return accepted_; }
  [[nodiscard]] std::uint64_t rejected(Verdict v) const noexcept { return rejected_[static_cast<int>(v)]; }
  [[nodiscard]] const RiskLimits& limits() const noexcept { return limits_; }

 private:
  void refill(std::uint64_t ts) noexcept;
  Verdict reject(Verdict v, std::uint64_t ts) noexcept;

  RiskLimits limits_;
  TripReason trip_{TripReason::None};
  // Token bucket in fixed point: 1 token = 1e9 units, refilled by
  // refill_per_second units per ns -- exact integer arithmetic.
  std::uint64_t tokens_;
  std::uint64_t last_ts_{0};
  bool have_ts_{false};
  // Reject-storm window.
  std::uint64_t window_start_{0};
  std::uint32_t window_rejects_{0};
  std::uint64_t accepted_{0};
  std::uint64_t rejected_[9]{};
};

}  // namespace lob::risk
