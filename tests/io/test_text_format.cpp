// Text format v1 (DESIGN.md §6.3): round trips at value extremes, and one
// test per class of malformed input, asserting the reported line and reason.

#include <gtest/gtest.h>

#include <limits>
#include <string>
#include <vector>

#include "../engine/engine_test_util.hpp"
#include "lob/io/text_format.hpp"

namespace lob::io {
namespace {

constexpr std::int64_t kI64Min = std::numeric_limits<std::int64_t>::min();
constexpr std::int64_t kI64Max = std::numeric_limits<std::int64_t>::max();
constexpr std::uint64_t kU64Max = std::numeric_limits<std::uint64_t>::max();

::testing::AssertionResult parse_fails(const std::function<void()>& parse, std::size_t line,
                                       const std::string& needle) {
  try {
    parse();
  } catch (const ParseError& e) {
    const std::string what = e.what();
    if (e.line() != line || what.find(needle) == std::string::npos) {
      return ::testing::AssertionFailure() << "got '" << what << "' (line " << e.line()
                                           << "), expected line " << line << " containing '" << needle << "'";
    }
    return ::testing::AssertionSuccess() << what;
  }
  return ::testing::AssertionFailure() << "parsed without error; expected '" << needle << "'";
}

TEST(TextFormat, EveryEventTypeRoundTripsAtExtremes) {
  const std::vector<Event> events{
      OrderAccepted{.seq = kU64Max, .id = kU64Max, .side = Side::Buy, .price = kI64Max, .quantity = kI64Max},
      OrderAccepted{.seq = 1, .id = 1, .side = Side::Sell, .price = kI64Min, .quantity = kI64Min},
      OrderRejected{.seq = 2, .command = CommandType::NewOrder, .id = 0, .reason = RejectReason::InvalidSide},
      OrderRejected{.seq = 3, .command = CommandType::CancelOrder, .id = 9, .reason = RejectReason::BookFull},
      TradeExecuted{.seq = 4, .taker_id = 5, .maker_id = 6, .taker_side = Side::Sell, .price = -7,
                    .quantity = 8, .taker_remaining = 0, .maker_remaining = kI64Max},
      OrderRested{.seq = 9, .id = 10, .side = Side::Buy, .price = 0, .remaining = 1},
      OrderCancelled{.seq = 11, .id = 12, .side = Side::Sell, .price = 13, .remaining = 14,
                     .reason = CancelReason::Requested},
      OrderCancelled{.seq = 15, .id = 16, .side = Side::Buy, .price = 17, .remaining = 18,
                     .reason = CancelReason::InsufficientResources},
  };
  for (const Event& e : events) {
    const std::string text = format_event(e);
    EXPECT_EQ(parse_event(text, 1), e) << text;
    EXPECT_EQ(format_event(parse_event(text, 1)), text);  // canonical: one spelling
  }
}

TEST(TextFormat, EveryRejectReasonSpellingIsParsed) {
  for (int r = 0; r < 8; ++r) {
    const Event e = OrderRejected{.seq = 1, .command = CommandType::NewOrder, .id = 1,
                                  .reason = static_cast<RejectReason>(r)};
    EXPECT_EQ(parse_event(format_event(e), 1), e);
  }
}

TEST(TextFormat, CommandsRoundTrip) {
  const std::vector<SequencedCommand> commands{
      {.cmd_seq = 1, .command = NewOrder{.id = 7, .side = Side::Buy, .price = 100, .quantity = 5}},
      {.cmd_seq = kU64Max, .command = NewOrder{.id = 0, .side = Side::Sell, .price = kI64Min, .quantity = -1}},
      {.cmd_seq = 3, .command = CancelOrder{.id = kU64Max}},
  };
  for (const auto& c : commands) {
    EXPECT_EQ(parse_command(format_command(c), 1), c);
  }
  EXPECT_EQ(format_command(commands[0]), "NEW 1 7 BUY 100 5");
  EXPECT_EQ(format_command(commands[2]), "CANCEL 3 18446744073709551615");
}

TEST(TextFormat, ConfigRoundTripAndValidation) {
  const EngineConfig c{.min_price = 1, .max_price = 60, .max_order_quantity = 100, .max_live_orders = 40};
  EXPECT_EQ(format_config(c), "CONFIG min_price=1 max_price=60 max_order_quantity=100 max_live_orders=40");
  EXPECT_EQ(parse_config(format_config(c), 1), c);
  EXPECT_TRUE(parse_fails([] { (void)parse_config("CONFIG min_price=0 max_price=60 max_order_quantity=100 max_live_orders=40", 4); },
                          4, "min_price must be >= 1"));
  EXPECT_TRUE(parse_fails([] { (void)parse_config("CONFIG max_price=60 min_price=1 max_order_quantity=100 max_live_orders=40", 2); },
                          2, "expected min_price=<value>"));
  EXPECT_TRUE(parse_fails([] { (void)parse_config("CONFIG min_price=1 max_price=60 max_order_quantity=100", 2); },
                          2, "expects 4 fields"));
}

TEST(TextFormat, NumbersAreStrictAndCanonical) {
  struct Case {
    std::string line;
    std::string needle;
  };
  const std::vector<Case> cases{
      {"NEW 1 1 BUY 007 5", "leading zero"},
      {"NEW 1 1 BUY +7 5", "not a decimal number"},
      {"NEW 1 1 BUY -0 5", "'-0' is not canonical"},
      {"NEW 1 1 BUY 9223372036854775808 5", "out of range"},
      {"NEW 1 1 BUY -9223372036854775809 5", "out of range"},
      {"NEW 1 18446744073709551616 BUY 1 5", "out of range"},
      {"NEW 1 -1 BUY 1 5", "not a decimal number"},
      {"NEW 1 1 BUY 1e3 5", "not a decimal number"},
      {"NEW 0 1 BUY 1 5", "cmd_seq must be >= 1"},
  };
  for (const Case& c : cases) {
    EXPECT_TRUE(parse_fails([&] { (void)parse_command(c.line, 7); }, 7, c.needle)) << c.line;
  }
  EXPECT_EQ(std::get<NewOrder>(parse_command("NEW 1 1 BUY -9223372036854775808 0", 1).command).price, kI64Min);
}

TEST(TextFormat, FieldSeparationIsStrict) {
  struct Case {
    std::string line;
    std::string needle;
  };
  const std::vector<Case> cases{
      {"NEW 1 1  BUY 1 5", "empty field"},
      {" NEW 1 1 BUY 1 5", "empty field"},
      {"NEW 1 1 BUY 1 5 ", "empty field"},
      {"NEW 1 1 BUY 1 5\r", "carriage return"},
      {"NEW\t1 1 BUY 1 5", "control or non-ASCII byte"},
      {"NEW 1 1 BUY 1", "expects 5 fields"},
      {"NEW 1 1 HOLD 1 5", "side must be BUY or SELL"},
      {"NEW 1 1 buy 1 5", "side must be BUY or SELL"},
      {"MODIFY 1 1", "unknown command"},
  };
  for (const Case& c : cases) {
    EXPECT_TRUE(parse_fails([&] { (void)parse_command(c.line, 3); }, 3, c.needle)) << c.line;
  }
}

TEST(TextFormat, MalformedEventsAreRejected) {
  EXPECT_TRUE(parse_fails([] { (void)parse_event("FILLED 1 2 3", 5); }, 5, "unknown event"));
  EXPECT_TRUE(parse_fails([] { (void)parse_event("REJECTED 1 NEW 2 NOT_A_REASON", 5); }, 5, "unknown reject reason"));
  EXPECT_TRUE(parse_fails([] { (void)parse_event("REJECTED 1 AMEND 2 BOOK_FULL", 5); }, 5, "NEW or CANCEL"));
  EXPECT_TRUE(parse_fails([] { (void)parse_event("CANCELLED 1 2 BUY 3 4 TIMEOUT", 5); }, 5, "cancel reason"));
  EXPECT_TRUE(parse_fails([] { (void)parse_event("TRADE 1 2 3 BUY 4 5 6", 5); }, 5, "expects 8 fields"));
}

TEST(CommandFile, ParsesHeaderCommentsAndCommands) {
  const std::string text =
      "LOB-COMMANDS v1\n"
      "# comment before config\n"
      "\n"
      "CONFIG min_price=1 max_price=100 max_order_quantity=10 max_live_orders=5\n"
      "NEW 1 7 SELL 50 3\n"
      "# comment between commands\n"
      "CANCEL 2 7\n";
  const CommandFile f = parse_command_file(text);
  EXPECT_EQ(f.config.max_price, 100);
  ASSERT_EQ(f.commands.size(), 2U);
  EXPECT_EQ(f.commands[1], (SequencedCommand{.cmd_seq = 2, .command = CancelOrder{.id = 7}}));
  // format -> parse is the identity (without comments)
  EXPECT_EQ(parse_command_file(format_command_file(f.config, f.commands)).commands, f.commands);
}

TEST(CommandFile, StructuralErrorsReportTheLine) {
  const std::string config = "CONFIG min_price=1 max_price=100 max_order_quantity=10 max_live_orders=5\n";
  struct Case {
    std::string text;
    std::size_t line;
    std::string needle;
  };
  const std::vector<Case> cases{
      {"", 1, "empty file"},
      {"LOB-COMMANDS v2\n" + config, 1, "unsupported format version"},
      {"lob commands\n" + config, 1, "missing header"},
      {"LOB-COMMANDS v1\nNEW 1 1 BUY 1 1\n", 2, "expected CONFIG line"},
      {"LOB-COMMANDS v1\n# only a comment\n", 2, "missing CONFIG line"},
      {"LOB-COMMANDS v1\n" + config + "NEW 2 1 BUY 1 1\n", 3, "cmd_seq 2 out of order (expected 1)"},
      {"LOB-COMMANDS v1\n" + config + "NEW 1 1 BUY 1 1\nCANCEL 3 1\n", 4, "cmd_seq 3 out of order (expected 2)"},
      {"LOB-COMMANDS v1\n" + config + "NEW 1 1 BUY 1 1", 3, "not terminated by '\\n'"},
      {"LOB-COMMANDS v1\r\n" + config, 1, "unsupported format version"},
  };
  for (const Case& c : cases) {
    EXPECT_TRUE(parse_fails([&] { (void)parse_command_file(c.text); }, c.line, c.needle)) << c.needle;
  }
}

}  // namespace
}  // namespace lob::io
