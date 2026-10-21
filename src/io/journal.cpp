#include "lob/io/journal.hpp"

#include <variant>

#include "lob/io/checksum.hpp"

namespace lob::io {

JournalCorruption::JournalCorruption(std::size_t offset, std::size_t line, const std::string& message)
    : std::runtime_error("journal corrupt at byte " + std::to_string(offset) + " (line " +
                         std::to_string(line) + "): " + message),
      offset_(offset),
      line_(line) {}

std::string encode_journal_header(const EngineConfig& config) {
  return std::string(kJournalMagic) + '\n' + format_config(config) + '\n';
}

std::string encode_batch(const SequencedCommand& command, std::span<const Event> events) {
  const std::string cmd = std::to_string(command.cmd_seq);
  std::string body = "BATCH " + cmd + ' ' + std::to_string(events.size()) + ' ' + format_command(command) + '\n';
  for (const Event& e : events) {
    body += format_event(e);
    body += '\n';
  }
  return body + "COMMIT " + cmd + ' ' + to_hex16(fnv1a64(body)) + '\n';
}

namespace {

/// Reads lines from a byte range, tracking offsets and 1-based line numbers.
class LineReader {
 public:
  LineReader(std::string_view bytes, std::size_t offset, std::size_t line_no)
      : bytes_(bytes), offset_(offset), line_no_(line_no) {}

  /// Next complete line (without '\n'); nullopt if the remaining bytes do not
  /// contain a '\n' (an incomplete last line, or nothing).
  std::optional<std::string_view> next() {
    const std::size_t nl = bytes_.find('\n', offset_);
    if (nl == std::string_view::npos) {
      return std::nullopt;
    }
    line_start_ = offset_;
    const std::string_view line = bytes_.substr(offset_, nl - offset_);
    offset_ = nl + 1;
    ++line_no_;
    return line;
  }

  [[nodiscard]] std::size_t offset() const { return offset_; }
  [[nodiscard]] std::size_t line_no() const { return line_no_; }
  [[nodiscard]] std::size_t line_start() const { return line_start_; }

 private:
  std::string_view bytes_;
  std::size_t offset_;
  std::size_t line_no_;
  std::size_t line_start_ = 0;
};

struct Incomplete {};
struct Invalid {
  std::size_t offset;
  std::size_t line;
  std::string message;
};
using BatchResult = std::variant<JournalBatch, Incomplete, Invalid>;

BatchResult parse_batch(std::string_view bytes, LineReader& in, CmdSeq expected_cmd,
                        SeqNum expected_seq) {
  const std::size_t begin = in.offset();
  const auto header = in.next();
  if (!header) {
    return Incomplete{};
  }
  const std::size_t header_line = in.line_no();
  auto invalid = [&](const std::string& message) {
    return Invalid{in.line_start(), in.line_no(), message};
  };
  try {
    // BATCH <cmd_seq> <n> <command line>   (the command line contains spaces)
    if (header->substr(0, 6) != "BATCH ") {
      return invalid("expected BATCH line");
    }
    const std::size_t s1 = header->find(' ', 6);
    const std::size_t s2 = s1 == std::string_view::npos ? s1 : header->find(' ', s1 + 1);
    if (s2 == std::string_view::npos) {
      return invalid("malformed BATCH line");
    }
    const CmdSeq cmd = parse_u64(header->substr(6, s1 - 6), header_line, "batch cmd_seq");
    const std::uint64_t n = parse_u64(header->substr(s1 + 1, s2 - s1 - 1), header_line, "event count");
    JournalBatch batch;
    batch.begin_offset = begin;
    batch.begin_line = header_line;
    batch.command = parse_command(header->substr(s2 + 1), header_line);
    if (batch.command.cmd_seq != cmd) {
      return invalid("BATCH cmd_seq " + std::to_string(cmd) + " != command cmd_seq " +
                     std::to_string(batch.command.cmd_seq));
    }
    if (cmd != expected_cmd) {
      return invalid("cmd_seq " + std::to_string(cmd) + " out of order (expected " +
                     std::to_string(expected_cmd) + ")");
    }
    if (n == 0) {
      return invalid("a batch must contain at least one event");
    }
    for (std::uint64_t i = 0; i < n; ++i) {
      const auto line = in.next();
      if (!line) {
        return Incomplete{};
      }
      if (line->substr(0, 7) == "COMMIT ") {
        return invalid("COMMIT after " + std::to_string(i) + " of " + std::to_string(n) + " events");
      }
      batch.events.push_back(parse_event(*line, in.line_no()));
      const SeqNum seq = seq_of(batch.events.back());
      if (seq != expected_seq) {
        return invalid("event seq " + std::to_string(seq) + " out of order (expected " +
                       std::to_string(expected_seq) + ")");
      }
      ++expected_seq;
    }
    const std::size_t commit_begin = in.offset();
    const auto commit = in.next();
    if (!commit) {
      return Incomplete{};
    }
    const auto f = split_fields(*commit, in.line_no());
    if (f.size() != 3 || f[0] != "COMMIT") {
      return invalid("expected COMMIT line after " + std::to_string(n) + " events");
    }
    if (parse_u64(f[1], in.line_no(), "commit cmd_seq") != cmd) {
      return invalid("COMMIT cmd_seq does not match BATCH");
    }
    const std::string expected_hash = to_hex16(fnv1a64(bytes.substr(begin, commit_begin - begin)));
    if (f[2] != expected_hash) {
      return invalid("checksum mismatch: stored " + std::string(f[2]) + ", computed " + expected_hash);
    }
    batch.end_offset = in.offset();
    return batch;
  } catch (const ParseError& e) {
    return Invalid{in.line_start(), e.line(), e.what()};
  }
}

}  // namespace

JournalScan scan_journal(std::string_view bytes) {
  JournalScan scan;
  LineReader in(bytes, 0, 0);

  // Header: magic + CONFIG. A crash while writing it leaves an incomplete
  // header, which is treated like an empty journal.
  const auto magic = in.next();
  const auto config = magic ? in.next() : std::nullopt;
  if (!magic || !config) {
    if (magic && *magic != kJournalMagic) {
      throw JournalCorruption(0, 1, "missing header '" + std::string(kJournalMagic) + "'");
    }
    scan.tail = bytes.empty() ? TailState::Clean : TailState::Torn;
    scan.dropped_bytes = bytes.size();
    return scan;
  }
  if (*magic != kJournalMagic) {
    throw JournalCorruption(0, 1, "missing or unsupported header '" + std::string(*magic) +
                                      "' (expected '" + std::string(kJournalMagic) + "')");
  }
  try {
    scan.config = parse_config(*config, 2);
  } catch (const ParseError& e) {
    throw JournalCorruption(in.line_start(), 2, e.what());
  }
  scan.valid_bytes = in.offset();

  CmdSeq next_cmd = 1;
  SeqNum next_seq = 1;
  while (in.offset() < bytes.size()) {
    BatchResult r = parse_batch(bytes, in, next_cmd, next_seq);
    if (auto* batch = std::get_if<JournalBatch>(&r)) {
      next_cmd = batch->command.cmd_seq + 1;
      next_seq = batch->last_seq() + 1;
      scan.valid_bytes = batch->end_offset;
      scan.batches.push_back(std::move(*batch));
      continue;
    }
    if (std::holds_alternative<Incomplete>(r)) {
      scan.tail = TailState::Torn;
      scan.dropped_bytes = bytes.size() - scan.valid_bytes;
      return scan;
    }
    const Invalid& bad = std::get<Invalid>(r);
    throw JournalCorruption(bad.offset, bad.line, bad.message);
  }
  return scan;
}

}  // namespace lob::io
