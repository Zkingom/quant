#pragma once

// M5: Session — the commit protocol around the matching engine
// (DESIGN.md §6.4).
//
// For every command: (1) apply in memory, (2) persist its batch with one
// ByteSink::write (+ sync in durable mode) — the commit point, (3) only then
// return the events to the caller (acknowledgement). The three steps are not
// atomic; the session makes the gaps safe:
//   - (1) succeeded but (2) failed  -> Poisoned: nothing is acknowledged and
//     every later call is refused; the only way on is to discard the session
//     and recover from the journal (the uncommitted result never becomes
//     visible);
//   - a command whose batch is committed is never executed again: re-
//     submitting its cmd_seq returns Duplicate (content-checked);
//   - a command that did not take effect (prepare-phase failure) can be
//     retried; the session retries transient bad_alloc a few times itself.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "lob/engine/matching_engine.hpp"
#include "lob/io/recovery.hpp"
#include "lob/io/sink.hpp"
#include "lob/io/snapshot_file.hpp"

namespace lob::io {

enum class SubmitStatus {
  Committed,                     ///< applied, persisted; events available
  CommittedWithResourceFailure,  ///< applied (trades stand), remainder cancelled
                                 ///< (INSUFFICIENT_RESOURCES), persisted
  Duplicate,                     ///< cmd_seq already committed with this content: not executed again
  ContentMismatch,               ///< cmd_seq already committed with DIFFERENT content: refused
  SequenceGap,                   ///< cmd_seq != last committed + 1: refused, nothing changed
  NotApplied,                    ///< transient failure before any effect (bad_alloc, retries exhausted)
  Refused,                       ///< deterministic failure before any effect (length/overflow_error)
  Poisoned,                      ///< session unusable; recover from the journal
};

[[nodiscard]] std::string_view to_string(SubmitStatus status) noexcept;

struct SubmitResult {
  SubmitStatus status = SubmitStatus::Poisoned;
  SeqNum first_seq = 0;  ///< Committed*/Duplicate: the batch's seq range
  SeqNum last_seq = 0;
  std::string detail;    ///< reason for Refused / ContentMismatch / SequenceGap (Poisoned: see poison_reason())
};

struct SessionOptions {
  bool durable = false;     ///< sync (fsync) after every batch, before acknowledging
  int prepare_retries = 2;  ///< extra attempts after a prepare-phase bad_alloc
};

class Session {
 public:
  /// `sink` must append right after recovered.report.valid_bytes (FileSink
  /// truncates to it). Writes the journal header if the journal has none.
  /// Throws if that header cannot be written.
  Session(const Recovered& recovered, ByteSink& sink, SessionOptions options);

  /// Never executes a committed command twice. Once a batch is persisted,
  /// recording it cannot fail (no allocation after the commit point).
  /// May throw std::bad_alloc only while composing the `detail` text of a
  /// refusal, i.e. when nothing has changed.
  SubmitResult submit(const SequencedCommand& command);

  /// Events of the most recent Committed* result (the acknowledgement).
  [[nodiscard]] std::span<const Event> last_events() const noexcept { return events_; }

  /// sync() the sink (end of a non-durable run). Poisons on failure.
  void sync();

  [[nodiscard]] bool poisoned() const noexcept { return poisoned_; }
  [[nodiscard]] const std::string& poison_reason() const noexcept { return poison_reason_; }
  [[nodiscard]] CmdSeq last_committed() const noexcept { return last_cmd_; }

  /// State at the last commit point. Throws std::logic_error when poisoned:
  /// the in-memory state may be ahead of the journal and must not be exposed.
  [[nodiscard]] SnapshotFile snapshot_file() const;

 private:
  SubmitResult poison(const char* what, const char* cause) noexcept;

  MatchingEngine engine_;
  ByteSink& sink_;
  SessionOptions options_;
  CmdSeq last_cmd_;
  std::vector<CommittedCommand> committed_;
  std::vector<Event> events_;
  bool poisoned_ = false;
  std::string poison_reason_;
};

}  // namespace lob::io
