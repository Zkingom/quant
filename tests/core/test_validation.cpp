#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "lob/core/validation.hpp"

namespace lob {
namespace {

constexpr std::int64_t kI64Max = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kI64Min = std::numeric_limits<std::int64_t>::min();

// A narrow band makes off-by-one errors at both edges easy to see.
constexpr EngineConfig kConfig{
    .min_price = 100, .max_price = 200, .max_order_quantity = 1'000, .max_live_orders = 1'000};

constexpr NewOrder valid_order() {
  return NewOrder{.id = 1, .side = Side::Buy, .price = 150, .quantity = 10};
}

constexpr NewOrder with_price(Price price) {
  NewOrder o = valid_order();
  o.price = price;
  return o;
}

constexpr NewOrder with_quantity(Quantity quantity) {
  NewOrder o = valid_order();
  o.quantity = quantity;
  return o;
}

// Compile-time spot checks: validation is usable in constant expressions.
static_assert(!validate_new_order(valid_order(), kConfig).has_value());
static_assert(validate_new_order(NewOrder{}, kConfig) == RejectReason::InvalidOrderId);
static_assert(validate_new_order(with_price(99), kConfig) == RejectReason::PriceOutOfRange);
static_assert(validate_new_order(with_quantity(1'001), kConfig) == RejectReason::QuantityTooLarge);
static_assert(validate_cancel_order(CancelOrder{.id = 1}) == std::nullopt);

TEST(ValidateNewOrder, AcceptsValidOrderOnBothSides) {
  NewOrder order = valid_order();
  EXPECT_EQ(validate_new_order(order, kConfig), std::nullopt);
  order.side = Side::Sell;
  EXPECT_EQ(validate_new_order(order, kConfig), std::nullopt);
}

TEST(ValidateNewOrder, OrderIdBoundaries) {
  NewOrder order = valid_order();
  order.id = 0;
  EXPECT_EQ(validate_new_order(order, kConfig), RejectReason::InvalidOrderId);
  order.id = 1;
  EXPECT_EQ(validate_new_order(order, kConfig), std::nullopt);
  order.id = std::numeric_limits<OrderId>::max();
  EXPECT_EQ(validate_new_order(order, kConfig), std::nullopt);
}

TEST(ValidateNewOrder, RejectsUnrepresentableSide) {
  for (const int raw : {2, 3, 127, 255}) {
    NewOrder order = valid_order();
    order.side = static_cast<Side>(raw);
    EXPECT_EQ(validate_new_order(order, kConfig), RejectReason::InvalidSide) << "raw=" << raw;
  }
}

struct Case {
  std::int64_t value;
  std::optional<RejectReason> expected;
};

TEST(ValidateNewOrder, PriceBandBoundaries) {
  const std::vector<Case> cases{
      {kI64Min, RejectReason::PriceOutOfRange},
      {-1, RejectReason::PriceOutOfRange},
      {0, RejectReason::PriceOutOfRange},
      {99, RejectReason::PriceOutOfRange},  // min - 1
      {100, std::nullopt},                  // min
      {101, std::nullopt},
      {199, std::nullopt},
      {200, std::nullopt},                  // max
      {201, RejectReason::PriceOutOfRange},  // max + 1
      {kI64Max, RejectReason::PriceOutOfRange},
  };
  for (const Case& c : cases) {
    EXPECT_EQ(validate_new_order(with_price(c.value), kConfig), c.expected) << "price=" << c.value;
  }
}

TEST(ValidateNewOrder, QuantityBoundaries) {
  const std::vector<Case> cases{
      {kI64Min, RejectReason::InvalidQuantity},
      {-1, RejectReason::InvalidQuantity},
      {0, RejectReason::InvalidQuantity},
      {1, std::nullopt},
      {999, std::nullopt},
      {1'000, std::nullopt},                    // max
      {1'001, RejectReason::QuantityTooLarge},  // max + 1
      {kI64Max, RejectReason::QuantityTooLarge},
  };
  for (const Case& c : cases) {
    EXPECT_EQ(validate_new_order(with_quantity(c.value), kConfig), c.expected) << "qty=" << c.value;
  }
}

TEST(ValidateNewOrder, ReportsFirstViolationInDocumentedOrder) {
  // Start with every field invalid, then fix fields one at a time in check
  // order; the reported reason must advance accordingly.
  NewOrder order{.id = 0, .side = static_cast<Side>(9), .price = 0, .quantity = 0};
  EXPECT_EQ(validate_new_order(order, kConfig), RejectReason::InvalidOrderId);
  order.id = 1;
  EXPECT_EQ(validate_new_order(order, kConfig), RejectReason::InvalidSide);
  order.side = Side::Sell;
  EXPECT_EQ(validate_new_order(order, kConfig), RejectReason::PriceOutOfRange);
  order.price = 150;
  EXPECT_EQ(validate_new_order(order, kConfig), RejectReason::InvalidQuantity);
  order.quantity = 1'001;
  EXPECT_EQ(validate_new_order(order, kConfig), RejectReason::QuantityTooLarge);
  order.quantity = 1;
  EXPECT_EQ(validate_new_order(order, kConfig), std::nullopt);
}

TEST(ValidateNewOrder, DefaultConstructedOrderIsRejected) {
  EXPECT_EQ(validate_new_order(NewOrder{}, kConfig), RejectReason::InvalidOrderId);
  EXPECT_EQ(validate_new_order(NewOrder{.id = 1}, kConfig), RejectReason::PriceOutOfRange);
  EXPECT_EQ(validate_new_order(NewOrder{.id = 1, .price = 150}, kConfig),
            RejectReason::InvalidQuantity);
}

TEST(ValidateNewOrder, SinglePriceBand) {
  constexpr EngineConfig single{
      .min_price = 7, .max_price = 7, .max_order_quantity = 1, .max_live_orders = 1};
  EXPECT_EQ(validate_new_order(NewOrder{.id = 1, .price = 7, .quantity = 1}, single), std::nullopt);
  EXPECT_EQ(validate_new_order(NewOrder{.id = 1, .price = 6, .quantity = 1}, single),
            RejectReason::PriceOutOfRange);
  EXPECT_EQ(validate_new_order(NewOrder{.id = 1, .price = 8, .quantity = 1}, single),
            RejectReason::PriceOutOfRange);
  EXPECT_EQ(validate_new_order(NewOrder{.id = 1, .price = 7, .quantity = 2}, single),
            RejectReason::QuantityTooLarge);
}

TEST(ValidateNewOrder, DefaultConfigBoundaries) {
  constexpr EngineConfig config{};
  NewOrder order{.id = 1, .side = Side::Sell, .price = config.max_price,
                 .quantity = config.max_order_quantity};
  EXPECT_EQ(validate_new_order(order, config), std::nullopt);
  order.price = config.min_price;
  EXPECT_EQ(validate_new_order(order, config), std::nullopt);
}

TEST(ValidateCancelOrder, Boundaries) {
  EXPECT_EQ(validate_cancel_order(CancelOrder{}), RejectReason::InvalidOrderId);
  EXPECT_EQ(validate_cancel_order(CancelOrder{.id = 0}), RejectReason::InvalidOrderId);
  EXPECT_EQ(validate_cancel_order(CancelOrder{.id = 1}), std::nullopt);
  EXPECT_EQ(validate_cancel_order(CancelOrder{.id = std::numeric_limits<OrderId>::max()}),
            std::nullopt);
}

}  // namespace
}  // namespace lob
