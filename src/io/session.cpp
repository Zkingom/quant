#include "lob/io/session.hpp"

#include <new>
#include <stdexcept>
#include <variant>

#include "lob/io/journal.hpp"

namespace lob::io {

std::string_view to_string(SubmitStatus status) noexcept {
  switch (status) {
    case SubmitStatus::Committed:
      return "COMMITTED";
    case SubmitStatus::CommittedWithResourceFailure:
      return "COMMITTED_WITH_RESOURCE_FAILURE";
    case SubmitStatus::Duplicate:
      return "DUPLICATE";
    case SubmitStatus::ContentMismatch:
      return "CMD_SEQ_REUSED";
    case SubmitStatus::SequenceGap:
      return "CMD_SEQ_GAP";
    case SubmitStatus::NotApplied:
      return "NOT_APPLIED";
    case SubmitStatus::Refused:
      return "REFUSED";
    case SubmitStatus::Poisoned:
      return "POISONED";
  }
  return "UNKNOWN";
}

Session::Session(const Recovered& recovered, ByteSink& sink, SessionOptions options)
    : engine_(MatchingEngine::restore(recovered.state)),
      sink_(sink),
      options_(options),
      last_cmd_(recovered.last_cmd),
      committed_(recovered.committed) {
  if (!recovered.report.journal_has_header) {
    sink_.write(encode_journal_header(recovered.state.config));
    if (options_.durable) {
      sink_.sync();
    }
  }
}

SubmitResult Session::poison(const char* what, const char* cause) noexcept {
  // The flag first: building the message may itself fail for lack of memory,
  // and the session must be poisoned regardless.
  poisoned_ = true;
  events_.clear();
  try {
    poison_reason_ = std::string(what) + cause;
  } catch (...) {
    poison_reason_.clear();  // best effort; poisoned() is what matters
  }
  return SubmitResult{.status = SubmitStatus::Poisoned, .first_seq = 0, .last_seq = 0, .detail = {}};
}

SubmitResult Session::submit(const SequencedCommand& command) {
  if (poisoned_) {
    return SubmitResult{.status = SubmitStatus::Poisoned, .first_seq = 0, .last_seq = 0, .detail = {}};
  }
  events_.clear();

  // Everything that can fail for lack of memory and is needed after the
  // commit point is done now, before any effect: the commit record must be
  // stored without allocating once the batch is durable, otherwise a failure
  // between "persisted" and "recorded" would let a retry execute the
  // command a second time.
  std::uint64_t digest = 0;
  try {
    digest = command_digest(command);
    committed_.reserve(committed_.size() + 1);
  } catch (const std::bad_alloc&) {
    return SubmitResult{.status = SubmitStatus::NotApplied, .first_seq = 0, .last_seq = 0, .detail = {}};
  }

  // Identity checks: never execute a committed command twice.
  if (command.cmd_seq >= 1 && command.cmd_seq <= last_cmd_) {
    if (command.cmd_seq <= committed_.size()) {
      const CommittedCommand& c = committed_[command.cmd_seq - 1];
      if (c.digest != digest) {
        return SubmitResult{.status = SubmitStatus::ContentMismatch,
                            .detail = "cmd_seq " + std::to_string(command.cmd_seq) +
                                      " was committed with different content"};
      }
      return SubmitResult{
          .status = SubmitStatus::Duplicate, .first_seq = c.first_seq, .last_seq = c.last_seq, .detail = {}};
    }
    return SubmitResult{.status = SubmitStatus::Duplicate, .first_seq = 0, .last_seq = 0, .detail = {}};
  }
  if (command.cmd_seq != last_cmd_ + 1) {
    return SubmitResult{.status = SubmitStatus::SequenceGap,
                        .detail = "cmd_seq " + std::to_string(command.cmd_seq) + ", expected " +
                                  std::to_string(last_cmd_ + 1)};
  }

  // (1) Apply. last_seq() before/after classifies a failure (DESIGN Q1).
  bool resource_failure = false;
  for (int attempt = 0;; ++attempt) {
    const SeqNum before = engine_.last_seq();
    events_.clear();
    try {
      engine_.process(command.command, events_);
      break;
    } catch (const std::bad_alloc&) {
      if (engine_.last_seq() != before) {
        resource_failure = true;  // RESIDUAL: applied; events end with the resource cancel
        break;
      }
      if (attempt >= options_.prepare_retries) {
        events_.clear();
        return SubmitResult{.status = SubmitStatus::NotApplied, .first_seq = 0, .last_seq = 0, .detail = {}};
      }
    } catch (const std::length_error& e) {
      if (engine_.last_seq() != before) {
        return poison("length_error after the command took effect: ", e.what());
      }
      events_.clear();
      return SubmitResult{.status = SubmitStatus::Refused, .first_seq = 0, .last_seq = 0, .detail = e.what()};
    } catch (const std::overflow_error& e) {
      if (engine_.last_seq() != before) {
        return poison("overflow_error after the command took effect: ", e.what());
      }
      events_.clear();
      return SubmitResult{.status = SubmitStatus::Refused, .first_seq = 0, .last_seq = 0, .detail = e.what()};
    } catch (const std::exception& e) {
      return poison("engine failure (state untrusted): ", e.what());
    } catch (...) {
      return poison("engine failure (state untrusted): ", "unknown exception");
    }
  }

  // (2) Persist: the commit point.
  try {
    const std::string batch = encode_batch(command, events_);
    sink_.write(batch);
    if (options_.durable) {
      sink_.sync();
    }
  } catch (const std::exception& e) {
    return poison("journal write failed (command applied in memory, not committed): ", e.what());
  } catch (...) {
    return poison("journal write failed: ", "unknown exception");
  }

  // (3) Committed: record (no allocation: capacity reserved above), then
  // acknowledge. Nothing below can throw.
  last_cmd_ = command.cmd_seq;
  const SeqNum first = seq_of(events_.front());
  const SeqNum last = seq_of(events_.back());
  committed_.push_back(CommittedCommand{.digest = digest,
                                        .first_seq = first,
                                        .last_seq = last,
                                        .resource_failure = resource_failure});
  return SubmitResult{.status = resource_failure ? SubmitStatus::CommittedWithResourceFailure
                                                 : SubmitStatus::Committed,
                      .first_seq = first,
                      .last_seq = last,
                      .detail = {}};
}

void Session::sync() {
  if (poisoned_) {
    return;
  }
  try {
    sink_.sync();
  } catch (const std::exception& e) {
    poison("journal sync failed: ", e.what());
  }
}

SnapshotFile Session::snapshot_file() const {
  if (poisoned_) {
    throw std::logic_error("Session::snapshot_file: session is poisoned (" + poison_reason_ + ")");
  }
  return SnapshotFile{.state = engine_.snapshot(), .last_cmd = last_cmd_};
}

}  // namespace lob::io
