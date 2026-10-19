#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

#include "lob/core/checked.hpp"

namespace lob {
namespace {

using i64 = std::int64_t;
using u64 = std::uint64_t;
constexpr i64 kMax = std::numeric_limits<i64>::max();
constexpr i64 kMin = std::numeric_limits<i64>::min();

// Compile-time checks double as documentation of the edge cases.
static_assert(checked_add<i64>(kMax - 1, 1) == kMax);
static_assert(!checked_add<i64>(kMax, 1).has_value());
static_assert(!checked_add<i64>(kMin, -1).has_value());
static_assert(checked_sub<i64>(kMin + 1, 1) == kMin);
static_assert(!checked_sub<i64>(kMin, 1).has_value());
static_assert(!checked_sub<i64>(0, kMin).has_value());  // -INT64_MIN is not representable
static_assert(!checked_mul<i64>(kMin, -1).has_value());
static_assert(!checked_sub<u64>(0, 1).has_value());     // no unsigned wrap-around
static_assert(!checked_add<u64>(std::numeric_limits<u64>::max(), 1).has_value());

TEST(Checked, AddBoundaries) {
  EXPECT_EQ(checked_add<i64>(2, 3), 5);
  EXPECT_EQ(checked_add<i64>(kMax, 0), kMax);
  EXPECT_EQ(checked_add<i64>(kMax, kMin), -1);
  EXPECT_FALSE(checked_add<i64>(kMax, kMax));
  EXPECT_FALSE(checked_add<i64>(kMin, kMin));
}

TEST(Checked, SubBoundaries) {
  EXPECT_EQ(checked_sub<i64>(5, 7), -2);
  EXPECT_EQ(checked_sub<i64>(kMax, kMax), 0);
  EXPECT_FALSE(checked_sub<i64>(kMax, -1));
  EXPECT_FALSE(checked_sub<i64>(kMin, 1));
}

TEST(Checked, MulBoundaries) {
  EXPECT_EQ(checked_mul<i64>(-4, 5), -20);
  EXPECT_EQ(checked_mul<i64>(kMax, 1), kMax);
  EXPECT_EQ(checked_mul<i64>(kMin, 1), kMin);
  EXPECT_EQ(checked_mul<i64>(kMax, -1), kMin + 1);
  EXPECT_FALSE(checked_mul<i64>(kMin, -1));
  EXPECT_FALSE(checked_mul<i64>(i64{1} << 32, i64{1} << 31));  // exactly 2^63
  EXPECT_EQ(checked_mul<i64>(i64{1} << 31, i64{1} << 31), i64{1} << 62);
}

TEST(Checked, NarrowTypes) {
  EXPECT_EQ(checked_add<std::int8_t>(100, 27), std::int8_t{127});
  EXPECT_FALSE(checked_add<std::int8_t>(100, 28));
  EXPECT_FALSE(checked_mul<std::uint8_t>(16, 16));
}

}  // namespace
}  // namespace lob
