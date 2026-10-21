#include "lob/io/verify.hpp"

#include <algorithm>
#include <variant>
#include <vector>

#include "lob/engine/matching_engine.hpp"
#include "lob/io/recovery.hpp"

namespace lob::io {

namespace {

std::string describe_batch(const JournalBatch& b, const std::vector<Event>& reexecuted) {
  std::string msg = "batch " + std::to_string(b.command.cmd_seq) + " (" + format_command(b.command) +
                    ")\n  journal:";
  for (const Event& e : b.events) {
    msg += "\n    " + format_event(e);
  }
  msg += "\n  re-executed:";
  for (const Event& e : reexecuted) {
    msg += "\n    " + format_event(e);
  }
  return msg;
}

}  // namespace

VerifyResult verify_journal(const JournalScan& scan) {
  VerifyResult result;
  const EngineConfig& config = *scan.config;
  MatchingEngine engine{config};
  EventFold fold{EngineSnapshot{.config = config, .last_seq = 0, .orders = {}}};
  std::vector<Event> events;
  for (const JournalBatch& b : scan.batches) {
    ++result.batches;
    events.clear();
    engine.process(b.command.command, events);
    const auto* rc = std::get_if<OrderCancelled>(&b.events.back());
    if (rc != nullptr && rc->reason == CancelReason::InsufficientResources) {
      const bool prefix_equal = events.size() == b.events.size() &&
                                std::equal(b.events.begin(), b.events.end() - 1, events.begin()) &&
                                std::holds_alternative<OrderRested>(events.back()) &&
                                std::get<OrderRested>(events.back()).remaining == rc->remaining &&
                                seq_of(events.back()) == rc->seq;
      if (!prefix_equal) {
        result.divergence = "differs before the resource-failure point: " + describe_batch(b, events);
        return result;
      }
      fold.apply(b);
      engine = MatchingEngine::restore(fold.snapshot());
      ++result.outcome_from_journal;
      continue;
    }
    if (events != b.events) {
      result.divergence = "re-execution differs: " + describe_batch(b, events);
      return result;
    }
    fold.apply(b);
    ++result.identical;
  }
  result.final_state = engine.snapshot();
  if (!(result.final_state == fold.snapshot())) {
    result.divergence = "final re-executed state differs from the journal state";
  }
  return result;
}

}  // namespace lob::io
