#pragma once

// M5: re-execution check of a journal (lob_replay verify; M4 contract C3).
//
// Every journaled command is executed again on a fresh engine and its events
// are compared with the journal. A batch that ended in INSUFFICIENT_RESOURCES
// cannot be reproduced (on re-execution the remainder rests), so for it only
// the part before the failure point is compared, the journal's outcome is
// taken as authoritative, and the engine is re-synchronised from the journal
// state (EventFold -> MatchingEngine::restore).

#include <cstddef>
#include <optional>
#include <string>

#include "lob/engine/snapshot.hpp"
#include "lob/io/journal.hpp"

namespace lob::io {

struct VerifyResult {
  std::size_t batches = 0;
  std::size_t identical = 0;             ///< re-execution produced exactly the journaled events
  std::size_t outcome_from_journal = 0;  ///< resource-failure batches (prefix verified)
  std::optional<std::string> divergence;  ///< first mismatch, if any
  EngineSnapshot final_state;            ///< state after the last batch (when no divergence)
};

/// Precondition: scan.config has a value.
VerifyResult verify_journal(const JournalScan& scan);

}  // namespace lob::io
