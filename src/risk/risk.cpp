#include "lob/risk/risk.hpp"

#include <algorithm>

namespace lob::risk {

namespace {
constexpr std::uint64_t kUnit = 1'000'000'000ULL;  // fixed-point units per token
}

std::string_view to_string(Verdict v) noexcept {
  switch (v) {
    case Verdict::Accept: return "ACCEPT";
    case Verdict::KillSwitch: return "KILL_SWITCH";
    case Verdict::NoReference: return "NO_REFERENCE";
    case Verdict::FatFingerQty: return "FAT_FINGER_QTY";
    case Verdict::FatFingerNotional: return "FAT_FINGER_NOTIONAL";
    case Verdict::PriceBand: return "PRICE_BAND";
    case Verdict::MaxPosition: return "MAX_POSITION";
    case Verdict::RateLimit: return "RATE_LIMIT";
  }
  return "?";
}

std::string_view to_string(TripReason r) noexcept {
  switch (r) {
    case TripReason::None: return "NONE";
    case TripReason::Manual: return "MANUAL";
    case TripReason::StaleBook: return "STALE_BOOK";
    case TripReason::RejectStorm: return "REJECT_STORM";
  }
  return "?";
}

RiskGate::RiskGate(const RiskLimits& limits) : limits_(limits), tokens_(limits.bucket_capacity * kUnit) {}

void RiskGate::reset() noexcept {
  trip_ = TripReason::None;
  window_rejects_ = 0;
}

void RiskGate::refill(std::uint64_t ts) noexcept {
  if (!have_ts_) {
    have_ts_ = true;
    last_ts_ = ts;
    return;
  }
  if (ts <= last_ts_) {
    return;  // market time never runs backwards here (the feed enforces it)
  }
  const std::uint64_t cap = limits_.bucket_capacity * kUnit;
  const std::uint64_t elapsed = ts - last_ts_;
  last_ts_ = ts;
  // elapsed * rate can overflow for very long gaps: saturate first.
  const std::uint64_t full_after = limits_.refill_per_second == 0 ? UINT64_MAX : cap / limits_.refill_per_second + 1;
  tokens_ = elapsed >= full_after ? cap : std::min(cap, tokens_ + elapsed * limits_.refill_per_second);
}

Verdict RiskGate::reject(Verdict v, std::uint64_t ts) noexcept {
  ++rejected_[static_cast<int>(v)];
  if (v == Verdict::KillSwitch) {
    return v;
  }
  if (ts - window_start_ > limits_.reject_window_ns || window_rejects_ == 0) {
    window_start_ = ts;
    window_rejects_ = 0;
  }
  if (++window_rejects_ >= limits_.max_rejects_in_window) {
    trip(TripReason::RejectStorm);
  }
  return v;
}

Verdict RiskGate::check_new(Side side, std::uint32_t price, std::uint32_t qty, const Exposure& ex,
                            const Reference& ref, std::uint64_t ts) noexcept {
  refill(ts);
  if (tripped()) {
    return reject(Verdict::KillSwitch, ts);
  }
  if (!ref.live || ref.bid == 0 || ref.ask == 0) {
    return reject(Verdict::NoReference, ts);
  }
  if (qty == 0 || qty > limits_.max_order_qty) {
    return reject(Verdict::FatFingerQty, ts);
  }
  if (static_cast<std::uint64_t>(price) * qty > limits_.max_notional) {
    return reject(Verdict::FatFingerNotional, ts);
  }
  const std::uint64_t mid = (static_cast<std::uint64_t>(ref.bid) + ref.ask) / 2;
  const std::uint64_t dist = price > mid ? price - mid : mid - price;
  if (dist * 10'000ULL > static_cast<std::uint64_t>(limits_.band_bps) * mid) {
    return reject(Verdict::PriceBand, ts);
  }
  const auto q = static_cast<std::int64_t>(qty);
  if (side == Side::Buy) {
    if (ex.position + static_cast<std::int64_t>(ex.open_buy) + q > limits_.max_position) {
      return reject(Verdict::MaxPosition, ts);
    }
  } else if (ex.position - static_cast<std::int64_t>(ex.open_sell) - q < -limits_.max_position) {
    return reject(Verdict::MaxPosition, ts);
  }
  if (tokens_ < kUnit) {
    return reject(Verdict::RateLimit, ts);
  }
  tokens_ -= kUnit;
  ++accepted_;
  return Verdict::Accept;
}

}  // namespace lob::risk
