#include "lob/io/text_format.hpp"

#include <array>
#include <limits>
#include <optional>
#include <variant>

#include "lob/core/checked.hpp"
#include "lob/io/checksum.hpp"

namespace lob::io {

std::string to_hex16(std::uint64_t value) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i) {
    out[static_cast<std::size_t>(i)] = kDigits[value & 0xF];
    value >>= 4;
  }
  return out;
}

ParseError::ParseError(std::size_t line, const std::string& message)
    : std::runtime_error("line " + std::to_string(line) + ": " + message), line_(line) {}

namespace {

[[noreturn]] void fail(std::size_t line_no, const std::string& message) {
  throw ParseError(line_no, message);
}

std::string quoted(std::string_view s) { return "'" + std::string(s) + "'"; }

constexpr std::array<RejectReason, 8> kRejectReasons{
    RejectReason::InvalidOrderId,   RejectReason::InvalidSide,      RejectReason::PriceOutOfRange,
    RejectReason::InvalidQuantity,  RejectReason::QuantityTooLarge, RejectReason::DuplicateOrderId,
    RejectReason::UnknownOrderId,   RejectReason::BookFull};

Side parse_side(std::string_view token, std::size_t line_no) {
  if (token == "BUY") {
    return Side::Buy;
  }
  if (token == "SELL") {
    return Side::Sell;
  }
  fail(line_no, "side must be BUY or SELL, got " + quoted(token));
}

RejectReason parse_reject_reason(std::string_view token, std::size_t line_no) {
  for (const RejectReason r : kRejectReasons) {
    if (to_string(r) == token) {
      return r;
    }
  }
  fail(line_no, "unknown reject reason " + quoted(token));
}

CommandType parse_command_type(std::string_view token, std::size_t line_no) {
  if (token == "NEW") {
    return CommandType::NewOrder;
  }
  if (token == "CANCEL") {
    return CommandType::CancelOrder;
  }
  fail(line_no, "command type must be NEW or CANCEL, got " + quoted(token));
}

CancelReason parse_cancel_reason(std::string_view token, std::size_t line_no) {
  if (token == "REQUESTED") {
    return CancelReason::Requested;
  }
  if (token == "INSUFFICIENT_RESOURCES") {
    return CancelReason::InsufficientResources;
  }
  fail(line_no, "cancel reason must be REQUESTED or INSUFFICIENT_RESOURCES, got " + quoted(token));
}

void expect_fields(const std::vector<std::string_view>& f, std::size_t n, std::size_t line_no,
                   std::string_view record) {
  if (f.size() != n) {
    fail(line_no, std::string(record) + " expects " + std::to_string(n - 1) + " fields, got " +
                      std::to_string(f.size() - 1));
  }
}

std::int64_t parse_key_value(std::string_view field, std::string_view key, std::size_t line_no) {
  if (field.size() <= key.size() || field.substr(0, key.size()) != key || field[key.size()] != '=') {
    fail(line_no, "expected " + std::string(key) + "=<value>, got " + quoted(field));
  }
  return parse_i64(field.substr(key.size() + 1), line_no, key);
}

std::string side_text(Side side) { return std::string(to_string(side)); }

}  // namespace

// ------------------------------------------------------------------- numbers

std::uint64_t parse_u64(std::string_view token, std::size_t line_no, std::string_view field) {
  if (token.empty()) {
    fail(line_no, std::string(field) + ": empty number");
  }
  if (token.size() > 1 && token[0] == '0') {
    fail(line_no, std::string(field) + ": leading zero in " + quoted(token));
  }
  std::uint64_t value = 0;
  for (const char c : token) {
    if (c < '0' || c > '9') {
      fail(line_no, std::string(field) + ": not a decimal number: " + quoted(token));
    }
    const auto shifted = checked_mul<std::uint64_t>(value, 10);
    const auto next = shifted ? checked_add<std::uint64_t>(*shifted, static_cast<std::uint64_t>(c - '0'))
                              : std::nullopt;
    if (!next) {
      fail(line_no, std::string(field) + ": out of range: " + quoted(token));
    }
    value = *next;
  }
  return value;
}

std::int64_t parse_i64(std::string_view token, std::size_t line_no, std::string_view field) {
  const bool negative = !token.empty() && token[0] == '-';
  const std::string_view digits = negative ? token.substr(1) : token;
  if (negative && digits == "0") {
    fail(line_no, std::string(field) + ": '-0' is not canonical");
  }
  const std::uint64_t magnitude = parse_u64(digits, line_no, field);
  constexpr auto kMax = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  if (!negative) {
    if (magnitude > kMax) {
      fail(line_no, std::string(field) + ": out of range: " + quoted(token));
    }
    return static_cast<std::int64_t>(magnitude);
  }
  if (magnitude > kMax + 1) {
    fail(line_no, std::string(field) + ": out of range: " + quoted(token));
  }
  // -magnitude without signed overflow (magnitude may be 2^63).
  return magnitude == kMax + 1 ? std::numeric_limits<std::int64_t>::min()
                               : -static_cast<std::int64_t>(magnitude);
}

std::vector<std::string_view> split_fields(std::string_view line, std::size_t line_no) {
  for (const char c : line) {
    const auto u = static_cast<unsigned char>(c);
    if (c == '\r') {
      fail(line_no, "carriage return ('\\r') is not allowed (use '\\n' line ends)");
    }
    if (u < 0x20 || u >= 0x7f) {
      fail(line_no, "control or non-ASCII byte 0x" + to_hex16(u).substr(14) + " is not allowed");
    }
  }
  std::vector<std::string_view> fields;
  std::size_t start = 0;
  while (true) {
    const std::size_t space = line.find(' ', start);
    const std::string_view field = line.substr(start, space == std::string_view::npos ? std::string_view::npos : space - start);
    if (field.empty()) {
      fail(line_no, "empty field (leading, trailing or double space)");
    }
    fields.push_back(field);
    if (space == std::string_view::npos) {
      break;
    }
    start = space + 1;
  }
  return fields;
}

// -------------------------------------------------------------------- config

std::string format_config(const EngineConfig& c) {
  return "CONFIG min_price=" + std::to_string(c.min_price) + " max_price=" + std::to_string(c.max_price) +
         " max_order_quantity=" + std::to_string(c.max_order_quantity) +
         " max_live_orders=" + std::to_string(c.max_live_orders);
}

EngineConfig parse_config(std::string_view line, std::size_t line_no) {
  const auto f = split_fields(line, line_no);
  if (f[0] != "CONFIG") {
    fail(line_no, "expected CONFIG line, got " + quoted(f[0]));
  }
  expect_fields(f, 5, line_no, "CONFIG");
  EngineConfig c{.min_price = parse_key_value(f[1], "min_price", line_no),
                 .max_price = parse_key_value(f[2], "max_price", line_no),
                 .max_order_quantity = parse_key_value(f[3], "max_order_quantity", line_no),
                 .max_live_orders = parse_key_value(f[4], "max_live_orders", line_no)};
  try {
    validate_config(c);
  } catch (const std::invalid_argument& e) {
    fail(line_no, e.what());
  }
  return c;
}

// ------------------------------------------------------------------ commands

std::string format_command(const SequencedCommand& sc) {
  const std::string seq = std::to_string(sc.cmd_seq);
  if (const auto* o = std::get_if<NewOrder>(&sc.command)) {
    return "NEW " + seq + ' ' + std::to_string(o->id) + ' ' + side_text(o->side) + ' ' +
           std::to_string(o->price) + ' ' + std::to_string(o->quantity);
  }
  return "CANCEL " + seq + ' ' + std::to_string(std::get<CancelOrder>(sc.command).id);
}

SequencedCommand parse_command(std::string_view line, std::size_t line_no) {
  const auto f = split_fields(line, line_no);
  SequencedCommand sc;
  if (f[0] == "NEW") {
    expect_fields(f, 6, line_no, "NEW");
    sc.cmd_seq = parse_u64(f[1], line_no, "cmd_seq");
    sc.command = NewOrder{.id = parse_u64(f[2], line_no, "order id"),
                          .side = parse_side(f[3], line_no),
                          .price = parse_i64(f[4], line_no, "price"),
                          .quantity = parse_i64(f[5], line_no, "quantity")};
  } else if (f[0] == "CANCEL") {
    expect_fields(f, 3, line_no, "CANCEL");
    sc.cmd_seq = parse_u64(f[1], line_no, "cmd_seq");
    sc.command = CancelOrder{.id = parse_u64(f[2], line_no, "order id")};
  } else {
    fail(line_no, "unknown command " + quoted(f[0]) + " (expected NEW or CANCEL)");
  }
  if (sc.cmd_seq == 0) {
    fail(line_no, "cmd_seq must be >= 1");
  }
  return sc;
}

// -------------------------------------------------------------------- events

std::string format_event(const Event& event) {
  struct Formatter {
    std::string operator()(const OrderAccepted& e) const {
      return "ACCEPTED " + std::to_string(e.seq) + ' ' + std::to_string(e.id) + ' ' + side_text(e.side) +
             ' ' + std::to_string(e.price) + ' ' + std::to_string(e.quantity);
    }
    std::string operator()(const OrderRejected& e) const {
      return "REJECTED " + std::to_string(e.seq) + ' ' + std::string(to_string(e.command)) + ' ' +
             std::to_string(e.id) + ' ' + std::string(to_string(e.reason));
    }
    std::string operator()(const TradeExecuted& e) const {
      return "TRADE " + std::to_string(e.seq) + ' ' + std::to_string(e.taker_id) + ' ' +
             std::to_string(e.maker_id) + ' ' + side_text(e.taker_side) + ' ' + std::to_string(e.price) +
             ' ' + std::to_string(e.quantity) + ' ' + std::to_string(e.taker_remaining) + ' ' +
             std::to_string(e.maker_remaining);
    }
    std::string operator()(const OrderRested& e) const {
      return "RESTED " + std::to_string(e.seq) + ' ' + std::to_string(e.id) + ' ' + side_text(e.side) +
             ' ' + std::to_string(e.price) + ' ' + std::to_string(e.remaining);
    }
    std::string operator()(const OrderCancelled& e) const {
      return "CANCELLED " + std::to_string(e.seq) + ' ' + std::to_string(e.id) + ' ' + side_text(e.side) +
             ' ' + std::to_string(e.price) + ' ' + std::to_string(e.remaining) + ' ' +
             std::string(to_string(e.reason));
    }
  };
  return std::visit(Formatter{}, event);
}

Event parse_event(std::string_view line, std::size_t line_no) {
  const auto f = split_fields(line, line_no);
  const std::string_view kind = f[0];
  if (kind == "ACCEPTED") {
    expect_fields(f, 6, line_no, "ACCEPTED");
    return OrderAccepted{.seq = parse_u64(f[1], line_no, "seq"),
                         .id = parse_u64(f[2], line_no, "order id"),
                         .side = parse_side(f[3], line_no),
                         .price = parse_i64(f[4], line_no, "price"),
                         .quantity = parse_i64(f[5], line_no, "quantity")};
  }
  if (kind == "REJECTED") {
    expect_fields(f, 5, line_no, "REJECTED");
    return OrderRejected{.seq = parse_u64(f[1], line_no, "seq"),
                         .command = parse_command_type(f[2], line_no),
                         .id = parse_u64(f[3], line_no, "order id"),
                         .reason = parse_reject_reason(f[4], line_no)};
  }
  if (kind == "TRADE") {
    expect_fields(f, 9, line_no, "TRADE");
    return TradeExecuted{.seq = parse_u64(f[1], line_no, "seq"),
                         .taker_id = parse_u64(f[2], line_no, "taker id"),
                         .maker_id = parse_u64(f[3], line_no, "maker id"),
                         .taker_side = parse_side(f[4], line_no),
                         .price = parse_i64(f[5], line_no, "price"),
                         .quantity = parse_i64(f[6], line_no, "quantity"),
                         .taker_remaining = parse_i64(f[7], line_no, "taker remaining"),
                         .maker_remaining = parse_i64(f[8], line_no, "maker remaining")};
  }
  if (kind == "RESTED") {
    expect_fields(f, 6, line_no, "RESTED");
    return OrderRested{.seq = parse_u64(f[1], line_no, "seq"),
                       .id = parse_u64(f[2], line_no, "order id"),
                       .side = parse_side(f[3], line_no),
                       .price = parse_i64(f[4], line_no, "price"),
                       .remaining = parse_i64(f[5], line_no, "remaining")};
  }
  if (kind == "CANCELLED") {
    expect_fields(f, 7, line_no, "CANCELLED");
    return OrderCancelled{.seq = parse_u64(f[1], line_no, "seq"),
                          .id = parse_u64(f[2], line_no, "order id"),
                          .side = parse_side(f[3], line_no),
                          .price = parse_i64(f[4], line_no, "price"),
                          .remaining = parse_i64(f[5], line_no, "remaining"),
                          .reason = parse_cancel_reason(f[6], line_no)};
  }
  fail(line_no, "unknown event " + quoted(kind));
}

// ------------------------------------------------------------- command files

namespace {

void check_magic(std::string_view line, std::string_view magic, std::size_t line_no) {
  if (line == magic) {
    return;
  }
  const std::string_view family = magic.substr(0, magic.find(' '));
  if (line.substr(0, family.size()) == family) {
    fail(line_no, "unsupported format version " + quoted(line) + " (expected " + quoted(magic) + ")");
  }
  fail(line_no, "missing header " + quoted(magic) + ", got " + quoted(line));
}

}  // namespace

CommandFile parse_command_file(std::string_view text) {
  CommandFile file;
  std::size_t pos = 0;
  std::size_t line_no = 0;
  bool have_magic = false;
  bool have_config = false;
  while (pos < text.size()) {
    const std::size_t nl = text.find('\n', pos);
    ++line_no;
    if (nl == std::string_view::npos) {
      fail(line_no, "last line is not terminated by '\\n'");
    }
    const std::string_view line = text.substr(pos, nl - pos);
    pos = nl + 1;
    if (!have_magic) {
      check_magic(line, kCommandsMagic, line_no);
      have_magic = true;
      continue;
    }
    if (line.empty() || line[0] == '#') {
      continue;
    }
    if (!have_config) {
      file.config = parse_config(line, line_no);
      have_config = true;
      continue;
    }
    SequencedCommand sc = parse_command(line, line_no);
    const CmdSeq expected = file.commands.size() + 1;
    if (sc.cmd_seq != expected) {
      fail(line_no, "cmd_seq " + std::to_string(sc.cmd_seq) + " out of order (expected " +
                        std::to_string(expected) + ")");
    }
    file.commands.push_back(sc);
  }
  if (!have_magic) {
    fail(1, "empty file (missing header " + quoted(kCommandsMagic) + ")");
  }
  if (!have_config) {
    fail(line_no, "missing CONFIG line");
  }
  return file;
}

std::string format_command_file(const EngineConfig& config, std::span<const SequencedCommand> commands) {
  std::string out = std::string(kCommandsMagic) + '\n' + format_config(config) + '\n';
  for (const SequencedCommand& c : commands) {
    out += format_command(c);
    out += '\n';
  }
  return out;
}

}  // namespace lob::io
