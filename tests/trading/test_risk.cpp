// L3' stage 3: pre-trade risk. One named test per check, plus the order of
// checks and the kill-switch semantics.

#include "lob/risk/risk.hpp"

#include <gtest/gtest.h>

namespace {

using lob::risk::Exposure;
using lob::risk::Reference;
using lob::risk::RiskGate;
using lob::risk::RiskLimits;
using lob::risk::TripReason;
using lob::risk::Verdict;
using Side = lob::md::itch::Side;

constexpr Reference kRef{1'000'000, 1'000'200, true, true};  // mid 1 000 100 ($100.01), tradable
constexpr std::uint64_t kSec = 1'000'000'000ULL;

RiskLimits limits() {
  RiskLimits l;
  l.max_position = 1000;
  l.max_order_qty = 500;
  l.max_notional = 500ULL * 1'000'000ULL;  // 500 shares at $100
  l.band_bps = 100;
  l.bucket_capacity = 5;
  l.refill_per_second = 10;
  l.max_rejects_in_window = 1000;
  return l;
}

TEST(Risk, AcceptsAnOrdinaryOrder) {
  RiskGate g{limits()};
  EXPECT_EQ(g.check_new(Side::Buy, 1'000'000, 100, {}, kRef, kSec), Verdict::Accept);
  EXPECT_EQ(g.accepted(), 1U);
}

TEST(Risk, MaxPositionCountsPositionAndOpenOrders) {
  RiskGate g{limits()};
  EXPECT_EQ(g.check_new(Side::Buy, 1'000'000, 100, Exposure{900, 0, 0}, kRef, kSec), Verdict::Accept);
  EXPECT_EQ(g.check_new(Side::Buy, 1'000'000, 101, Exposure{900, 0, 0}, kRef, kSec), Verdict::MaxPosition);
  EXPECT_EQ(g.check_new(Side::Buy, 1'000'000, 100, Exposure{800, 101, 0}, kRef, kSec), Verdict::MaxPosition)
      << "open buys count toward the worst case";
  EXPECT_EQ(g.check_new(Side::Sell, 1'000'000, 100, Exposure{-900, 0, 1}, kRef, kSec), Verdict::MaxPosition);
  EXPECT_EQ(g.check_new(Side::Sell, 1'000'000, 100, Exposure{900, 5000, 0}, kRef, kSec), Verdict::Accept)
      << "a sell reduces a long position; open buys do not matter";
}

TEST(Risk, TokenBucketLimitsOrderRateAndRefillsByMarketTime) {
  RiskGate g{limits()};  // burst 5, 10 per second
  std::uint64_t t = 10 * kSec;
  for (int i = 0; i < 5; ++i) EXPECT_EQ(g.check_new(Side::Buy, 1'000'000, 1, {}, kRef, t), Verdict::Accept) << i;
  EXPECT_EQ(g.check_new(Side::Buy, 1'000'000, 1, {}, kRef, t), Verdict::RateLimit);
  t += kSec / 10 - 1;  // just under one token
  EXPECT_EQ(g.check_new(Side::Buy, 1'000'000, 1, {}, kRef, t), Verdict::RateLimit);
  t += 1;
  EXPECT_EQ(g.check_new(Side::Buy, 1'000'000, 1, {}, kRef, t), Verdict::Accept);
  t += 3600 * kSec;  // long gap: refill saturates at the burst size (no overflow)
  for (int i = 0; i < 5; ++i) EXPECT_EQ(g.check_new(Side::Buy, 1'000'000, 1, {}, kRef, t), Verdict::Accept) << i;
  EXPECT_EQ(g.check_new(Side::Buy, 1'000'000, 1, {}, kRef, t), Verdict::RateLimit);
}

TEST(Risk, PriceBandIsRelativeToTheMid) {
  RiskGate g{limits()};  // 1% of 1 000 100 = 10 001
  EXPECT_EQ(g.check_new(Side::Buy, 1'000'100 - 10'001, 1, {}, kRef, kSec), Verdict::Accept);
  EXPECT_EQ(g.check_new(Side::Buy, 1'000'100 - 10'002, 1, {}, kRef, kSec), Verdict::PriceBand);
  EXPECT_EQ(g.check_new(Side::Sell, 1'000'100 + 10'001, 1, {}, kRef, kSec), Verdict::Accept);
  EXPECT_EQ(g.check_new(Side::Sell, 1'000'100 + 10'002, 1, {}, kRef, kSec), Verdict::PriceBand);
}

TEST(Risk, FatFingerQuantity) {
  RiskGate g{limits()};
  EXPECT_EQ(g.check_new(Side::Buy, 1'000'000, 0, {}, kRef, kSec), Verdict::FatFingerQty);
  EXPECT_EQ(g.check_new(Side::Buy, 1'000'000, 501, {}, kRef, kSec), Verdict::FatFingerQty);
  EXPECT_EQ(g.check_new(Side::Buy, 1'000'000, 500, {}, kRef, kSec), Verdict::Accept);
}

TEST(Risk, FatFingerNotional) {
  RiskLimits l = limits();
  l.max_notional = 100ULL * 1'000'000ULL;  // 100 shares at $100
  RiskGate g{l};
  EXPECT_EQ(g.check_new(Side::Buy, 1'000'000, 100, {}, kRef, kSec), Verdict::Accept);
  EXPECT_EQ(g.check_new(Side::Buy, 1'000'001, 100, {}, kRef, kSec), Verdict::FatFingerNotional);
}

TEST(Risk, NoReferenceWhenTheBookIsOneSided) {
  RiskGate g{limits()};
  EXPECT_EQ(g.check_new(Side::Buy, 1'000'000, 1, {}, Reference{1'000'000, 0, true, true}, kSec), Verdict::NoReference);
  EXPECT_EQ(g.check_new(Side::Buy, 1'000'000, 1, {}, Reference{0, 1'000'200, true, true}, kSec), Verdict::NoReference);
}

TEST(Risk, NotTradingWhileHaltedOrOutsideRegularHours) {
  RiskGate g{limits()};
  EXPECT_EQ(g.check_new(Side::Buy, 1'000'000, 1, {}, Reference{1'000'000, 1'000'200, true, false}, kSec),
            Verdict::NotTrading);
  EXPECT_EQ(g.check_new(Side::Buy, 1'000'000, 1, {}, Reference{1'000'000, 1'000'200, false, false}, kSec),
            Verdict::NotTrading);
  EXPECT_FALSE(g.tripped()) << "a halt is not a reason for the kill switch";
  EXPECT_EQ(g.check_new(Side::Buy, 1'000'000, 1, {}, kRef, kSec), Verdict::Accept) << "resumes by itself";
}

TEST(Risk, NotTradingRejectsDoNotCountTowardTheRejectStorm) {
  RiskLimits l = limits();
  l.max_rejects_in_window = 3;
  RiskGate g{l};
  for (int i = 0; i < 100; ++i) {
    EXPECT_EQ(g.check_new(Side::Buy, 1'000'000, 1, {}, Reference{1'000'000, 1'000'200, true, false}, kSec),
              Verdict::NotTrading);
  }
  EXPECT_FALSE(g.tripped());
  EXPECT_EQ(g.rejected(Verdict::NotTrading), 100U);
}

TEST(Risk, KillSwitchBlocksNewOrdersAndIsStickyUntilReset) {
  RiskGate g{limits()};
  g.trip(TripReason::Manual);
  EXPECT_TRUE(g.tripped());
  EXPECT_EQ(g.check_new(Side::Buy, 1'000'000, 1, {}, kRef, kSec), Verdict::KillSwitch);
  g.trip(TripReason::StaleBook);
  EXPECT_EQ(g.trip_reason(), TripReason::Manual) << "the first reason is kept";
  EXPECT_EQ(g.check_new(Side::Buy, 1'000'000, 1, {}, kRef, 2 * kSec), Verdict::KillSwitch)
      << "still tripped although nothing is wrong any more";
  g.reset();
  EXPECT_EQ(g.check_new(Side::Buy, 1'000'000, 1, {}, kRef, 3 * kSec), Verdict::Accept);
}

TEST(Risk, RejectStormTripsTheKillSwitch) {
  RiskLimits l = limits();
  l.max_rejects_in_window = 3;
  l.reject_window_ns = kSec;
  RiskGate g{l};
  EXPECT_EQ(g.check_new(Side::Buy, 1, 1, {}, kRef, kSec), Verdict::PriceBand);
  EXPECT_EQ(g.check_new(Side::Buy, 1, 1, {}, kRef, kSec + 1), Verdict::PriceBand);
  EXPECT_FALSE(g.tripped());
  EXPECT_EQ(g.check_new(Side::Buy, 1, 1, {}, kRef, kSec + 2), Verdict::PriceBand);
  EXPECT_TRUE(g.tripped());
  EXPECT_EQ(g.trip_reason(), TripReason::RejectStorm);
}

TEST(Risk, RejectsOutsideTheWindowDoNotAccumulate) {
  RiskLimits l = limits();
  l.max_rejects_in_window = 3;
  l.reject_window_ns = kSec;
  RiskGate g{l};
  for (std::uint64_t i = 0; i < 10; ++i) {
    EXPECT_EQ(g.check_new(Side::Buy, 1, 1, {}, kRef, (i + 1) * 2 * kSec), Verdict::PriceBand);
  }
  EXPECT_FALSE(g.tripped());
}

TEST(Risk, ChecksRunInTheDocumentedOrder) {
  // An order violating everything reports the first check, and so on.
  RiskGate g{limits()};
  const Exposure huge{5000, 0, 0};
  EXPECT_EQ(g.check_new(Side::Buy, 1, 0, huge, Reference{0, 0, false, false}, kSec), Verdict::NotTrading);
  EXPECT_EQ(g.check_new(Side::Buy, 1, 0, huge, Reference{0, 0, true, true}, kSec), Verdict::NoReference);
  EXPECT_EQ(g.check_new(Side::Buy, 1, 0, huge, kRef, kSec), Verdict::FatFingerQty);
  EXPECT_EQ(g.check_new(Side::Buy, 2'000'000, 500, huge, kRef, kSec), Verdict::FatFingerNotional);
  EXPECT_EQ(g.check_new(Side::Buy, 1, 1, huge, kRef, kSec), Verdict::PriceBand);
  EXPECT_EQ(g.check_new(Side::Buy, 1'000'000, 1, huge, kRef, kSec), Verdict::MaxPosition);
  g.trip(TripReason::Manual);
  EXPECT_EQ(g.check_new(Side::Buy, 1, 0, huge, Reference{0, 0, false, false}, kSec), Verdict::KillSwitch);
  EXPECT_EQ(g.rejected(Verdict::PriceBand), 1U);
}

}  // namespace
