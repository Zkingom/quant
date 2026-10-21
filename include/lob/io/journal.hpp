#pragma once

// M5: the journal (event log) — the durable source of truth (DESIGN.md §6).
//
//   LOB-JOURNAL v1
//   CONFIG …
//   BATCH <cmd_seq> <n> <command line>      one batch per applied command
//   <event line> x n
//   COMMIT <cmd_seq> <fnv1a64 hex of the batch text up to this line>
//
// A batch is committed only if it is complete and verifies. scan_journal()
// validates every batch (framing, checksum, cmd_seq contiguous from 1, event
// seq contiguous from 1, the command echoed by its own events is consistent
// at the framing level) and classifies what follows the last good batch:
//   Clean   nothing follows;
//   Torn    only the beginning of a batch follows (no complete COMMIT line;
//           every complete line valid so far; the last line may be partial):
//           the remains of a write interrupted by a crash; dropped;
//   corrupt anything else: JournalCorruption is thrown.

#include <cstddef>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "lob/engine/events.hpp"
#include "lob/io/text_format.hpp"

namespace lob::io {

struct JournalBatch {
  SequencedCommand command;
  std::vector<Event> events;
  std::size_t begin_offset = 0;  ///< byte offset of the BATCH line
  std::size_t begin_line = 0;    ///< 1-based line number of the BATCH line
  std::size_t end_offset = 0;    ///< one past the COMMIT line's '\n'

  [[nodiscard]] SeqNum first_seq() const { return seq_of(events.front()); }
  [[nodiscard]] SeqNum last_seq() const { return seq_of(events.back()); }
};

enum class TailState { Clean, Torn };

struct JournalScan {
  std::optional<EngineConfig> config;  ///< nullopt: no complete header (empty or torn header)
  std::vector<JournalBatch> batches;
  TailState tail = TailState::Clean;
  std::size_t valid_bytes = 0;    ///< header + complete batches; keep exactly this many bytes
  std::size_t dropped_bytes = 0;  ///< torn tail (0 when Clean)
};

/// The journal is damaged in a way a crash cannot explain; automatic
/// recovery would lose or invent committed history, so it is refused.
class JournalCorruption : public std::runtime_error {
 public:
  JournalCorruption(std::size_t offset, std::size_t line, const std::string& message);
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
  [[nodiscard]] std::size_t line() const noexcept { return line_; }

 private:
  std::size_t offset_;
  std::size_t line_;
};

std::string encode_journal_header(const EngineConfig& config);

/// The complete text of one batch (written with a single ByteSink::write).
/// Precondition: events is non-empty.
std::string encode_batch(const SequencedCommand& command, std::span<const Event> events);

JournalScan scan_journal(std::string_view bytes);

[[nodiscard]] constexpr std::string_view to_string(TailState t) noexcept {
  return t == TailState::Clean ? "CLEAN" : "TORN";
}

}  // namespace lob::io
