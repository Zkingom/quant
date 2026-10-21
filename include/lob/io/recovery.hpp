#pragma once

// M5: recovery = snapshot (optional) + journal -> engine state at the last
// commit point (DESIGN.md §6.5). Events, not commands, are the source of
// truth, so histories with resource failures are recovered exactly.

#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "lob/engine/snapshot.hpp"
#include "lob/io/journal.hpp"
#include "lob/io/snapshot_file.hpp"

namespace lob::io {

/// Snapshot and journal do not describe one history (or configs disagree).
class RecoveryError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// Applies committed batches to an engine state, event by event, checking
/// that each batch is consistent with its own command and with the current
/// state (a TRADE names a resting maker on the other side at its price, a
/// RESTED order matches its ACCEPTED, ...). Throws JournalCorruption on any
/// inconsistency.
class EventFold {
 public:
  explicit EventFold(const EngineSnapshot& start);
  void apply(const JournalBatch& batch);
  /// Canonical snapshot of the current state.
  [[nodiscard]] EngineSnapshot snapshot() const;
  [[nodiscard]] SeqNum last_seq() const noexcept { return last_seq_; }

 private:
  struct Order {
    Side side;
    Price price;
    Quantity remaining;
    SeqNum seq;
  };
  EngineConfig config_;
  std::map<OrderId, Order> resting_;
  SeqNum last_seq_ = 0;
};

/// What the journal says about one committed command (index = cmd_seq - 1).
struct CommittedCommand {
  std::uint64_t digest = 0;  ///< fnv1a64 of the canonical command line
  SeqNum first_seq = 0;
  SeqNum last_seq = 0;
  bool resource_failure = false;  ///< batch ends with CANCELLED ... INSUFFICIENT_RESOURCES
};

struct RecoveryReport {
  bool snapshot_used = false;
  CmdSeq snapshot_cmd = 0;
  std::size_t batches_in_journal = 0;
  std::size_t batches_applied = 0;  ///< batches after the snapshot boundary
  TailState tail = TailState::Clean;
  std::size_t valid_bytes = 0;      ///< journal bytes to keep (truncate to this before appending)
  std::size_t dropped_bytes = 0;
  bool journal_has_header = false;
};

struct Recovered {
  EngineSnapshot state;  ///< validated (find_snapshot_violation) and canonical
  CmdSeq last_cmd = 0;   ///< commit watermark
  std::vector<CommittedCommand> committed;
  RecoveryReport report;
};

/// `expected_config` (e.g. from a command file) must match the journal and
/// snapshot configs when present; it is required when neither has one.
/// Throws JournalCorruption, RecoveryError, or ParseError.
Recovered recover(std::string_view journal_bytes, const std::optional<SnapshotFile>& snapshot,
                  const std::optional<EngineConfig>& expected_config);

std::uint64_t command_digest(const SequencedCommand& command);

}  // namespace lob::io
