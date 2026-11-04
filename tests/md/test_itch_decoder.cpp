// L3' 1b: ITCH 5.0 subset decoder and length-prefix framing.

#include "lob/md/itch.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "lob/testkit/itch_builder.hpp"

namespace {

namespace itch = lob::md::itch;
namespace tk = lob::testkit::itch;
using itch::DecodeStatus;
using itch::FrameStatus;

std::vector<std::uint8_t> hex(std::string_view s) {
  std::vector<std::uint8_t> out;
  int nibble = -1;
  for (const char ch : s) {
    int v = -1;
    if (ch >= '0' && ch <= '9') v = ch - '0';
    else if (ch >= 'a' && ch <= 'f') v = ch - 'a' + 10;
    else if (ch >= 'A' && ch <= 'F') v = ch - 'A' + 10;
    if (v < 0) continue;
    if (nibble < 0) {
      nibble = v;
    } else {
      out.push_back(static_cast<std::uint8_t>(nibble * 16 + v));
      nibble = -1;
    }
  }
  return out;
}

itch::Message decode_ok(const std::vector<std::uint8_t>& bytes) {
  itch::Message m;
  EXPECT_EQ(itch::decode(bytes, m), DecodeStatus::Decoded);
  return m;
}

std::string sym(const itch::Symbol& s) { return std::string(s.data(), s.size()); }

// --- One literal per message type, written by hand from the specification
// (field by field; comments give offsets). These pin both the decoder and
// the testkit builder to the spec independently of each other.

// S: type 'S' | locate 0 | tracking 0 | ts 1 | event 'O'
const char* const kSystemEvent = "53 0000 0000 000000000001 4F";
// R: 'R' | locate 1 | tracking 0 | ts 0 | "AAPL    " | Q | N | lot 100 | N | C | "Z " | P | N | ' ' | 1 | N | 0 | N
const char* const kStockDirectory =
    "52 0001 0000 000000000000 4141504C20202020 51 4E 00000064 4E 43 5A20 50 4E 20 31 4E 00000000 4E";
// A: 'A' | locate 1 | tracking 2 | ts 42 | ref 7 | 'B' | 300 | "AAPL    " | 1 000 000
const char* const kAddOrder =
    "41 0001 0002 00000000002A 0000000000000007 42 0000012C 4141504C20202020 000F4240";
// F: 'F' | locate 1 | tracking 0 | ts 42 | ref 9 | 'S' | 100 | "AAPL    " | 1 000 016 | "GSCO"
const char* const kAddOrderMpid =
    "46 0001 0000 00000000002A 0000000000000009 53 00000064 4141504C20202020 000F4250 4753434F";
// E: 'E' | locate 1 | 0 | ts 100 | ref 7 | 100 | match 0xABCD
const char* const kExecuted = "45 0001 0000 000000000064 0000000000000007 00000064 000000000000ABCD";
// C: 'C' | locate 1 | 0 | ts 101 | ref 7 | 50 | match 1 | 'Y' | 1 000 001
const char* const kExecutedPrice =
    "43 0001 0000 000000000065 0000000000000007 00000032 0000000000000001 59 000F4241";
// X: 'X' | locate 1 | 0 | ts 102 | ref 7 | 10
const char* const kCancel = "58 0001 0000 000000000066 0000000000000007 0000000A";
// D: 'D' | locate 1 | 0 | ts 103 | ref 7
const char* const kDelete = "44 0001 0000 000000000067 0000000000000007";
// U: 'U' | locate 1 | 0 | ts 104 | old 7 | new 8 | 200 | 1 000 000
const char* const kReplace = "55 0001 0000 000000000068 0000000000000007 0000000000000008 000000C8 000F4240";

TEST(ItchDecode, SystemEventLiteral) {
  const auto m = std::get<itch::SystemEvent>(decode_ok(hex(kSystemEvent)));
  EXPECT_EQ(m.header.type, 'S');
  EXPECT_EQ(m.header.timestamp, 1U);
  EXPECT_EQ(m.event_code, 'O');
  EXPECT_EQ(hex(kSystemEvent), tk::system_event('O', 1));
}

TEST(ItchDecode, StockDirectoryLiteral) {
  const auto bytes = hex(kStockDirectory);
  ASSERT_EQ(bytes.size(), 39U);
  const auto m = std::get<itch::StockDirectory>(decode_ok(bytes));
  EXPECT_EQ(m.header.stock_locate, 1U);
  EXPECT_EQ(sym(m.stock), "AAPL    ");
  EXPECT_EQ(m.market_category, 'Q');
  EXPECT_EQ(m.round_lot_size, 100U);
  EXPECT_EQ(m.issue_subtype[0], 'Z');
  EXPECT_EQ(m.luld_reference_price_tier, '1');
  EXPECT_EQ(m.inverse_indicator, 'N');
  EXPECT_EQ(bytes, tk::stock_directory(1, "AAPL"));
}

TEST(ItchDecode, AddOrderLiteral) {
  const auto bytes = hex(kAddOrder);
  ASSERT_EQ(bytes.size(), 36U);
  const auto m = std::get<itch::AddOrder>(decode_ok(bytes));
  EXPECT_EQ(m.header.stock_locate, 1U);
  EXPECT_EQ(m.header.tracking_number, 2U);
  EXPECT_EQ(m.header.timestamp, 42U);
  EXPECT_EQ(m.order_ref, 7U);
  EXPECT_EQ(m.side, itch::Side::Buy);
  EXPECT_EQ(m.shares, 300U);
  EXPECT_EQ(sym(m.stock), "AAPL    ");
  EXPECT_EQ(m.price, 1'000'000U);
  EXPECT_FALSE(m.has_mpid);
  auto built = tk::add_order(1, 7, 'B', 300, "AAPL", 1'000'000, 42);
  built[3] = 0;  // builder writes tracking 0; the literal uses 2
  built[4] = 2;
  EXPECT_EQ(bytes, built);
}

TEST(ItchDecode, AddOrderWithMpidLiteral) {
  const auto bytes = hex(kAddOrderMpid);
  ASSERT_EQ(bytes.size(), 40U);
  const auto m = std::get<itch::AddOrder>(decode_ok(bytes));
  EXPECT_EQ(m.order_ref, 9U);
  EXPECT_EQ(m.side, itch::Side::Sell);
  EXPECT_EQ(m.shares, 100U);
  EXPECT_EQ(m.price, 1'000'016U);
  EXPECT_TRUE(m.has_mpid);
  EXPECT_EQ(std::string(m.attribution.data(), 4), "GSCO");
  EXPECT_EQ(bytes, tk::add_order_mpid(1, 9, 'S', 100, "AAPL", 1'000'016, "GSCO", 42));
}

TEST(ItchDecode, OrderExecutedLiteral) {
  const auto bytes = hex(kExecuted);
  ASSERT_EQ(bytes.size(), 31U);
  const auto m = std::get<itch::OrderExecuted>(decode_ok(bytes));
  EXPECT_EQ(m.header.timestamp, 100U);
  EXPECT_EQ(m.order_ref, 7U);
  EXPECT_EQ(m.executed_shares, 100U);
  EXPECT_EQ(m.match_number, 0xABCDU);
  EXPECT_EQ(bytes, tk::order_executed(1, 7, 100, 0xABCD, 100));
}

TEST(ItchDecode, OrderExecutedWithPriceLiteral) {
  const auto bytes = hex(kExecutedPrice);
  ASSERT_EQ(bytes.size(), 36U);
  const auto m = std::get<itch::OrderExecutedWithPrice>(decode_ok(bytes));
  EXPECT_EQ(m.executed_shares, 50U);
  EXPECT_EQ(m.match_number, 1U);
  EXPECT_EQ(m.printable, 'Y');
  EXPECT_EQ(m.execution_price, 1'000'001U);
  EXPECT_EQ(bytes, tk::order_executed_price(1, 7, 50, 1, 'Y', 1'000'001, 101));
}

TEST(ItchDecode, OrderCancelLiteral) {
  const auto bytes = hex(kCancel);
  ASSERT_EQ(bytes.size(), 23U);
  const auto m = std::get<itch::OrderCancel>(decode_ok(bytes));
  EXPECT_EQ(m.order_ref, 7U);
  EXPECT_EQ(m.cancelled_shares, 10U);
  EXPECT_EQ(bytes, tk::order_cancel(1, 7, 10, 102));
}

TEST(ItchDecode, OrderDeleteLiteral) {
  const auto bytes = hex(kDelete);
  ASSERT_EQ(bytes.size(), 19U);
  const auto m = std::get<itch::OrderDelete>(decode_ok(bytes));
  EXPECT_EQ(m.order_ref, 7U);
  EXPECT_EQ(m.header.timestamp, 103U);
  EXPECT_EQ(bytes, tk::order_delete(1, 7, 103));
}

TEST(ItchDecode, OrderReplaceLiteral) {
  const auto bytes = hex(kReplace);
  ASSERT_EQ(bytes.size(), 35U);
  const auto m = std::get<itch::OrderReplace>(decode_ok(bytes));
  EXPECT_EQ(m.original_order_ref, 7U);
  EXPECT_EQ(m.new_order_ref, 8U);
  EXPECT_EQ(m.shares, 200U);
  EXPECT_EQ(m.price, 1'000'000U);
  EXPECT_EQ(bytes, tk::order_replace(1, 7, 8, 200, 1'000'000, 104));
}

// H: 'H' | locate 1 | 0 | ts 105 | "AAPL    " | state 'H' | reserved ' ' | reason "T1  "
const char* const kTradingAction = "48 0001 0000 000000000069 4141504C20202020 48 20 54312020";

TEST(ItchDecode, TradingActionLiteral) {
  const auto bytes = hex(kTradingAction);
  ASSERT_EQ(bytes.size(), 25U);
  const auto m = std::get<itch::TradingAction>(decode_ok(bytes));
  EXPECT_EQ(m.header.stock_locate, 1U);
  EXPECT_EQ(m.header.timestamp, 105U);
  EXPECT_EQ(sym(m.stock), "AAPL    ");
  EXPECT_EQ(m.trading_state, 'H');
  EXPECT_EQ(std::string(m.reason.data(), 4), "T1  ");
  EXPECT_EQ(bytes, tk::trading_action(1, "AAPL", 'H', 105, "T1"));
}

// Q: 'Q' | locate 1 | 0 | ts 106 | shares 11813 | "SXTC    " | price 58200 | match 7 | 'H'
const char* const kCrossTrade =
    "51 0001 0000 00000000006A 0000000000002E25 5358544320202020 0000E358 0000000000000007 48";

TEST(ItchDecode, CrossTradeLiteral) {
  const auto bytes = hex(kCrossTrade);
  ASSERT_EQ(bytes.size(), 40U);
  const auto m = std::get<itch::CrossTrade>(decode_ok(bytes));
  EXPECT_EQ(m.header.timestamp, 106U);
  EXPECT_EQ(m.shares, 11813U);
  EXPECT_EQ(sym(m.stock), "SXTC    ");
  EXPECT_EQ(m.cross_price, 58200U);
  EXPECT_EQ(m.match_number, 7U);
  EXPECT_EQ(m.cross_type, 'H');
  EXPECT_EQ(bytes, tk::cross_trade(1, "SXTC", 11813, 58200, 'H', 106, 7));
  itch::Message x;
  for (const char ct : std::string("OCHI")) {
    EXPECT_EQ(itch::decode(tk::cross_trade(1, "A", 1, 1, ct), x), itch::DecodeStatus::Decoded) << ct;
  }
  EXPECT_EQ(itch::decode(tk::cross_trade(1, "A", 1, 1, 'X'), x), itch::DecodeStatus::InvalidField);
}

TEST(ItchDecode, TradingStatesOutsideHPQTAreInvalid) {
  itch::Message m;
  for (const char st : std::string("HPQT")) {
    EXPECT_EQ(itch::decode(tk::trading_action(1, "A", st), m), itch::DecodeStatus::Decoded) << st;
  }
  for (const char st : std::string("hRX ")) {
    EXPECT_EQ(itch::decode(tk::trading_action(1, "A", st), m), itch::DecodeStatus::InvalidField) << st;
  }
}

TEST(ItchDecode, LargestFieldValuesDecodeWithoutSignIssues) {
  const auto bytes = tk::order_replace(0xffff, ~0ULL, ~0ULL - 1, 0xffffffffU, 0xffffffffU,
                                       itch::kNanosPerDay - 1);
  const auto m = std::get<itch::OrderReplace>(decode_ok(bytes));
  EXPECT_EQ(m.header.stock_locate, 0xffffU);
  EXPECT_EQ(m.header.timestamp, itch::kNanosPerDay - 1);
  EXPECT_EQ(m.original_order_ref, ~0ULL);
  EXPECT_EQ(m.new_order_ref, ~0ULL - 1);
  EXPECT_EQ(m.shares, 0xffffffffU);
  EXPECT_EQ(m.price, 0xffffffffU);
}

TEST(ItchDecode, EverySubsetTypeRejectsLengthOffByOne) {
  for (const char* lit : {kSystemEvent, kStockDirectory, kAddOrder, kAddOrderMpid, kExecuted,
                          kExecutedPrice, kCancel, kDelete, kReplace, kTradingAction, kCrossTrade}) {
    auto bytes = hex(lit);
    itch::Message m = itch::OrderDelete{};
    auto shorter = bytes;
    shorter.pop_back();
    EXPECT_EQ(itch::decode(shorter, m), DecodeStatus::BadLength) << lit;
    auto longer = bytes;
    longer.push_back(0);
    EXPECT_EQ(itch::decode(longer, m), DecodeStatus::BadLength) << lit;
    EXPECT_TRUE(std::holds_alternative<itch::OrderDelete>(m)) << "out must be untouched";
  }
}

TEST(ItchDecode, NonSubsetSpecTypesAreSkippedOnlyAtTheirExactLength) {
  const std::string non_subset = "YLVWKJhPBINO";
  for (const char t : non_subset) {
    const std::size_t len = itch::spec_length(t);
    ASSERT_GT(len, itch::kHeaderSize) << t;
    std::vector<std::uint8_t> bytes(len, 0);
    bytes[0] = static_cast<std::uint8_t>(t);
    itch::Message m;
    EXPECT_EQ(itch::decode(bytes, m), DecodeStatus::Skipped) << t;
    bytes.push_back(0);
    EXPECT_EQ(itch::decode(bytes, m), DecodeStatus::BadLength) << t;
  }
  itch::Message m;
  EXPECT_EQ(itch::decode(tk::trade_message(1), m), DecodeStatus::Skipped);
}

TEST(ItchDecode, EveryUndefinedTypeByteIsRejected) {
  const std::string defined = "SRHYLVWKJhAFECXDUPQBINO";
  int undefined = 0;
  for (int t = 0; t < 256; ++t) {
    const char c = static_cast<char>(t);
    std::vector<std::uint8_t> bytes(40, 0);
    bytes[0] = static_cast<std::uint8_t>(t);
    itch::Message m;
    const DecodeStatus s = itch::decode(bytes, m);
    if (defined.find(c) == std::string::npos) {
      EXPECT_EQ(s, DecodeStatus::UnknownType) << t;
      EXPECT_EQ(itch::spec_length(c), 0U);
      ++undefined;
    } else {
      EXPECT_NE(s, DecodeStatus::UnknownType) << t;
    }
  }
  EXPECT_EQ(undefined, 256 - 23);
}

TEST(ItchDecode, EnumeratedFieldsOutOfRangeAreInvalid) {
  itch::Message m;
  EXPECT_EQ(itch::decode(tk::add_order(1, 7, 'X', 1, "A", 1), m), DecodeStatus::InvalidField);
  EXPECT_EQ(itch::decode(tk::add_order_mpid(1, 7, 'b', 1, "A", 1, "MM"), m), DecodeStatus::InvalidField);
  EXPECT_EQ(itch::decode(tk::order_executed_price(1, 7, 1, 1, 'y', 1), m), DecodeStatus::InvalidField);
  EXPECT_EQ(itch::decode(tk::system_event('Z'), m), DecodeStatus::InvalidField);
  for (const char code : std::string("OSQMEC")) {
    EXPECT_EQ(itch::decode(tk::system_event(code), m), DecodeStatus::Decoded) << code;
  }
}

TEST(ItchDecode, TimestampMustBeWithinOneDay) {
  itch::Message m;
  EXPECT_EQ(itch::decode(tk::order_delete(1, 7, itch::kNanosPerDay - 1), m), DecodeStatus::Decoded);
  EXPECT_EQ(itch::decode(tk::order_delete(1, 7, itch::kNanosPerDay), m), DecodeStatus::InvalidField);
  EXPECT_EQ(itch::decode(tk::order_delete(1, 7, 0xffffffffffffULL), m), DecodeStatus::InvalidField);
}

TEST(ItchDecode, EmptyPayload) {
  itch::Message m;
  EXPECT_EQ(itch::decode({}, m), DecodeStatus::Empty);
}

TEST(ItchDecode, StatusNamesAreDistinct) {
  EXPECT_EQ(itch::to_string(DecodeStatus::BadLength), "BAD_LENGTH");
  EXPECT_EQ(itch::to_string(DecodeStatus::InvalidField), "INVALID_FIELD");
  EXPECT_EQ(itch::to_string(DecodeStatus::Skipped), "SKIPPED");
}

// --- Framing

TEST(ItchFrame, WalksConsecutiveFramesAndEndsExactly) {
  const auto stream = tk::frames({hex(kAddOrder), hex(kDelete), tk::trade_message(1)});
  itch::FrameCursor cur{stream};
  itch::Bytes p;
  ASSERT_EQ(cur.next(p), FrameStatus::Frame);
  EXPECT_EQ(p.size(), 36U);
  EXPECT_EQ(p.data(), stream.data() + 2) << "payload must alias the input buffer (zero-copy)";
  ASSERT_EQ(cur.next(p), FrameStatus::Frame);
  EXPECT_EQ(p.size(), 19U);
  ASSERT_EQ(cur.next(p), FrameStatus::Frame);
  EXPECT_EQ(p.size(), 44U);
  EXPECT_EQ(cur.next(p), FrameStatus::End);
  EXPECT_EQ(cur.offset(), stream.size());
}

TEST(ItchFrame, TruncatedPrefixOrBodyLeavesCursorInPlace) {
  const auto one = tk::frames({hex(kDelete)});
  for (std::size_t cut = 1; cut < one.size(); ++cut) {
    const std::vector<std::uint8_t> partial(one.begin(), one.begin() + static_cast<long>(cut));
    itch::FrameCursor cur{partial};
    itch::Bytes p;
    EXPECT_EQ(cur.next(p), FrameStatus::Truncated) << cut;
    EXPECT_EQ(cur.offset(), 0U);
  }
}

TEST(ItchFrame, ZeroLengthPrefixIsRejected) {
  const std::vector<std::uint8_t> stream{0, 0, 0x44};
  itch::FrameCursor cur{stream};
  itch::Bytes p;
  EXPECT_EQ(cur.next(p), FrameStatus::ZeroLength);
  EXPECT_EQ(cur.offset(), 0U);
}

TEST(ItchFrame, EmptyBufferIsEnd) {
  itch::FrameCursor cur{itch::Bytes{}};
  itch::Bytes p;
  EXPECT_EQ(cur.next(p), FrameStatus::End);
}

}  // namespace
