#include <gtest/gtest.h>

#include <array>
#include <set>
#include <string_view>
#include <variant>

#include "lob/core/command.hpp"
#include "lob/core/reject_reason.hpp"
#include "lob/core/types.hpp"

namespace lob {
namespace {

static_assert(opposite(Side::Buy) == Side::Sell);
static_assert(opposite(Side::Sell) == Side::Buy);
static_assert(is_valid(Side::Buy) && is_valid(Side::Sell));
static_assert(!is_valid(static_cast<Side>(2)));
static_assert(sizeof(Side) == 1);

TEST(Side, OppositeIsAnInvolution) {
  for (const Side s : {Side::Buy, Side::Sell}) {
    EXPECT_NE(opposite(s), s);
    EXPECT_EQ(opposite(opposite(s)), s);
  }
}

TEST(Side, OnlyTwoUnderlyingValuesAreValid) {
  for (int raw = 0; raw <= 255; ++raw) {
    const auto side = static_cast<Side>(raw);
    EXPECT_EQ(is_valid(side), raw == 0 || raw == 1) << "raw=" << raw;
  }
}

TEST(Side, StableStrings) {
  EXPECT_EQ(to_string(Side::Buy), "BUY");
  EXPECT_EQ(to_string(Side::Sell), "SELL");
  EXPECT_EQ(to_string(static_cast<Side>(42)), "INVALID");
}

TEST(RejectReason, StringsAreStableAndUnique) {
  // Golden files depend on these spellings; changing one must be deliberate.
  constexpr std::array<std::pair<RejectReason, std::string_view>, 8> expected{{
      {RejectReason::InvalidOrderId, "INVALID_ORDER_ID"},
      {RejectReason::InvalidSide, "INVALID_SIDE"},
      {RejectReason::PriceOutOfRange, "PRICE_OUT_OF_RANGE"},
      {RejectReason::InvalidQuantity, "INVALID_QUANTITY"},
      {RejectReason::QuantityTooLarge, "QUANTITY_TOO_LARGE"},
      {RejectReason::DuplicateOrderId, "DUPLICATE_ORDER_ID"},
      {RejectReason::UnknownOrderId, "UNKNOWN_ORDER_ID"},
      {RejectReason::BookFull, "BOOK_FULL"},
  }};
  std::set<std::string_view> seen;
  for (const auto& [reason, text] : expected) {
    EXPECT_EQ(to_string(reason), text);
    EXPECT_TRUE(seen.insert(text).second) << "duplicate string " << text;
  }
  // The enum is dense from 0; the value after the last enumerator must not
  // map to a real string. Fails if an enumerator is added without updating
  // this test.
  EXPECT_EQ(to_string(static_cast<RejectReason>(expected.size())), "UNKNOWN_REJECT_REASON");
}

TEST(Command, DefaultsAreInvalidSentinels) {
  constexpr NewOrder order{};
  EXPECT_EQ(order.id, kInvalidOrderId);
  EXPECT_EQ(order.price, 0);
  EXPECT_EQ(order.quantity, 0);
  EXPECT_EQ(CancelOrder{}.id, kInvalidOrderId);
}

TEST(Command, EqualityComparesAllFields) {
  constexpr NewOrder base{.id = 1, .side = Side::Buy, .price = 10, .quantity = 3};
  EXPECT_EQ(base, (NewOrder{.id = 1, .side = Side::Buy, .price = 10, .quantity = 3}));
  EXPECT_NE(base, (NewOrder{.id = 2, .side = Side::Buy, .price = 10, .quantity = 3}));
  EXPECT_NE(base, (NewOrder{.id = 1, .side = Side::Sell, .price = 10, .quantity = 3}));
  EXPECT_NE(base, (NewOrder{.id = 1, .side = Side::Buy, .price = 11, .quantity = 3}));
  EXPECT_NE(base, (NewOrder{.id = 1, .side = Side::Buy, .price = 10, .quantity = 4}));
}

TEST(Command, VariantHoldsEitherAlternative) {
  Command cmd = NewOrder{.id = 5, .side = Side::Sell, .price = 7, .quantity = 1};
  ASSERT_TRUE(std::holds_alternative<NewOrder>(cmd));
  EXPECT_EQ(std::get<NewOrder>(cmd).id, 5U);
  cmd = CancelOrder{.id = 5};
  ASSERT_TRUE(std::holds_alternative<CancelOrder>(cmd));
  EXPECT_EQ(std::get<CancelOrder>(cmd).id, 5U);
}

}  // namespace
}  // namespace lob
