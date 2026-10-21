#include "lob/io/snapshot_file.hpp"

#include <optional>

#include "lob/io/checksum.hpp"

namespace lob::io {

namespace {

std::string state_text(const EngineSnapshot& s) {
  std::string out = format_config(s.config) + '\n';
  out += "LAST_SEQ " + std::to_string(s.last_seq) + '\n';
  out += "ORDERS " + std::to_string(s.orders.size()) + '\n';
  for (const SnapshotOrder& o : s.orders) {
    out += "ORDER " + std::string(to_string(o.side)) + ' ' + std::to_string(o.price) + ' ' +
           std::to_string(o.id) + ' ' + std::to_string(o.remaining) + ' ' + std::to_string(o.seq) + '\n';
  }
  return out;
}

}  // namespace

std::uint64_t state_hash(const EngineSnapshot& state) { return fnv1a64(state_text(state)); }

std::string encode_snapshot(const SnapshotFile& snapshot) {
  const EngineSnapshot& s = snapshot.state;
  std::string body = std::string(kSnapshotMagic) + '\n' + format_config(s.config) + '\n';
  body += "LAST_CMD " + std::to_string(snapshot.last_cmd) + '\n';
  body += "LAST_SEQ " + std::to_string(s.last_seq) + '\n';
  body += "ORDERS " + std::to_string(s.orders.size()) + '\n';
  for (const SnapshotOrder& o : s.orders) {
    body += "ORDER " + std::string(to_string(o.side)) + ' ' + std::to_string(o.price) + ' ' +
            std::to_string(o.id) + ' ' + std::to_string(o.remaining) + ' ' + std::to_string(o.seq) + '\n';
  }
  return body + "END " + to_hex16(fnv1a64(body)) + '\n';
}

SnapshotFile parse_snapshot(std::string_view bytes) {
  std::size_t pos = 0;
  std::size_t line_no = 0;
  auto next = [&]() -> std::string_view {
    const std::size_t nl = bytes.find('\n', pos);
    ++line_no;
    if (nl == std::string_view::npos) {
      throw ParseError(line_no, "snapshot truncated (missing line or final '\\n')");
    }
    const std::string_view line = bytes.substr(pos, nl - pos);
    pos = nl + 1;
    return line;
  };
  auto keyed = [&](std::string_view line, std::string_view key) {
    const auto f = split_fields(line, line_no);
    if (f.size() != 2 || f[0] != key) {
      throw ParseError(line_no, "expected '" + std::string(key) + " <number>'");
    }
    return parse_u64(f[1], line_no, key);
  };

  if (const std::string_view magic = next(); magic != kSnapshotMagic) {
    throw ParseError(line_no, "missing or unsupported header '" + std::string(magic) + "' (expected '" +
                                  std::string(kSnapshotMagic) + "')");
  }
  SnapshotFile file;
  file.state.config = parse_config(next(), line_no);
  file.last_cmd = keyed(next(), "LAST_CMD");
  file.state.last_seq = keyed(next(), "LAST_SEQ");
  const std::uint64_t n = keyed(next(), "ORDERS");
  for (std::uint64_t i = 0; i < n; ++i) {
    const std::string_view line = next();
    const auto f = split_fields(line, line_no);
    if (f.size() != 6 || f[0] != "ORDER") {
      throw ParseError(line_no, "expected ORDER <side> <price> <id> <remaining> <seq>");
    }
    Side side = Side::Buy;
    if (f[1] == "SELL") {
      side = Side::Sell;
    } else if (f[1] != "BUY") {
      throw ParseError(line_no, "side must be BUY or SELL");
    }
    file.state.orders.push_back(SnapshotOrder{.id = parse_u64(f[3], line_no, "order id"),
                                              .side = side,
                                              .price = parse_i64(f[2], line_no, "price"),
                                              .remaining = parse_i64(f[4], line_no, "remaining"),
                                              .seq = parse_u64(f[5], line_no, "seq")});
  }
  const std::size_t body_end = pos;
  const auto end = split_fields(next(), line_no);
  if (end.size() != 2 || end[0] != "END") {
    throw ParseError(line_no, "expected END <checksum> after " + std::to_string(n) + " orders");
  }
  const std::string expected = to_hex16(fnv1a64(bytes.substr(0, body_end)));
  if (end[1] != expected) {
    throw ParseError(line_no, "checksum mismatch: stored " + std::string(end[1]) + ", computed " + expected);
  }
  if (pos != bytes.size()) {
    throw ParseError(line_no + 1, "unexpected bytes after END");
  }
  if (const auto violation = find_snapshot_violation(file.state)) {
    throw ParseError(line_no, "invalid snapshot state: " + *violation);
  }
  return file;
}

}  // namespace lob::io
