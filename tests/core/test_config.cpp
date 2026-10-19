#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

#include "lob/core/config.hpp"

namespace lob {
namespace {

constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();

// Asserts that validate_config throws std::invalid_argument and that the
// message names the offending constraint.
void expect_invalid(const EngineConfig& config, const std::string& needle) {
  try {
    validate_config(config);
    ADD_FAILURE() << "expected std::invalid_argument mentioning '" << needle << "'";
  } catch (const std::invalid_argument& e) {
    EXPECT_NE(std::string(e.what()).find(needle), std::string::npos) << e.what();
  }
}

TEST(EngineConfig, DefaultIsValid) {
  EXPECT_NO_THROW(validate_config(EngineConfig{}));
}

TEST(EngineConfig, MinPriceMustBePositive) {
  expect_invalid(EngineConfig{.min_price = 0}, "min_price");
  expect_invalid(EngineConfig{.min_price = -5}, "min_price");
  expect_invalid(EngineConfig{.min_price = std::numeric_limits<std::int64_t>::min()}, "min_price");
  EXPECT_NO_THROW(validate_config(EngineConfig{.min_price = 1}));
}

TEST(EngineConfig, PriceBandMustNotBeInverted) {
  expect_invalid(EngineConfig{.min_price = 10, .max_price = 9}, "max_price must be >= min_price");
  EXPECT_NO_THROW(validate_config(EngineConfig{.min_price = 10, .max_price = 10}));
}

TEST(EngineConfig, MaxOrderQuantityMustBePositive) {
  expect_invalid(EngineConfig{.max_order_quantity = 0}, "max_order_quantity must be >= 1");
  expect_invalid(EngineConfig{.max_order_quantity = -1}, "max_order_quantity must be >= 1");
}

TEST(EngineConfig, MaxLiveOrdersMustBePositive) {
  expect_invalid(EngineConfig{.max_live_orders = 0}, "max_live_orders must be >= 1");
  expect_invalid(EngineConfig{.max_live_orders = -1}, "max_live_orders must be >= 1");
}

// INT64_MAX = 49 * 188232082384791343, so the product bound can be hit exactly.
static_assert(kMax % 49 == 0);

TEST(EngineConfig, NotionalBoundIsExact) {
  const EngineConfig at_limit{
      .min_price = 1, .max_price = 49, .max_order_quantity = kMax / 49, .max_live_orders = 1};
  EXPECT_NO_THROW(validate_config(at_limit));

  EngineConfig over = at_limit;
  over.max_order_quantity += 1;
  expect_invalid(over, "max_price * max_order_quantity");
}

TEST(EngineConfig, AggregateQuantityBoundIsExact) {
  const EngineConfig at_limit{
      .min_price = 1, .max_price = 1, .max_order_quantity = kMax / 49, .max_live_orders = 49};
  EXPECT_NO_THROW(validate_config(at_limit));

  EngineConfig over = at_limit;
  over.max_live_orders = 50;
  expect_invalid(over, "max_live_orders * max_order_quantity");
}

TEST(EngineConfig, ChecksAreReportedInDocumentedOrder) {
  // Everything wrong: the first constraint (min_price) is reported.
  expect_invalid(EngineConfig{.min_price = 0, .max_price = -1, .max_order_quantity = 0,
                              .max_live_orders = 0},
                 "min_price must be >= 1");
  // Both product constraints violated: notional is reported first.
  expect_invalid(EngineConfig{.min_price = 1, .max_price = kMax, .max_order_quantity = kMax,
                              .max_live_orders = kMax},
                 "max_price * max_order_quantity");
}

}  // namespace
}  // namespace lob
