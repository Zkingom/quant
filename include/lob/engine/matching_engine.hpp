#pragma once

// M3: the matching engine.
//
// MatchingEngine turns commands into events and keeps the order book in
// price-time priority. It is single-threaded and deterministic: the same
// config and the same command sequence always produce the same events and
// the same book, independent of compiler and optimisation level.
//
// Processing a command has two phases (DESIGN.md §3.5):
//   prepare  read-only. All validation (including duplicate id and the
//            BOOK_FULL capacity rule, which needs a dry run of the match),
//            seq-range check and reservation of event capacity. If anything
//            throws here, neither the engine nor `events` has changed.
//   commit   append events into the reserved capacity, execute fills, rest
//            the remainder. The only step that can fail is resting the
//            remainder (std::bad_alloc inside OrderBook::add); then the
//            executed trades stand, the remainder is reported as
//            OrderCancelled{InsufficientResources}, and bad_alloc propagates.
//            That case has the basic guarantee, not the strong one; see
//            DESIGN.md §3.5 for why rollback is not possible.

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "lob/book/order_book.hpp"
#include "lob/core/command.hpp"
#include "lob/core/config.hpp"
#include "lob/core/types.hpp"
#include "lob/engine/events.hpp"
#include "lob/engine/snapshot.hpp"

namespace lob {

class MatchingEngine {
 public:
  /// Throws std::invalid_argument if the config is invalid.
  explicit MatchingEngine(const EngineConfig& config);

  // Not copyable (the book is not). Moves are noexcept; a moved-from engine
  // has an empty book and keeps its last seq.
  MatchingEngine(const MatchingEngine&) = delete;
  MatchingEngine& operator=(const MatchingEngine&) = delete;
  MatchingEngine(MatchingEngine&&) noexcept = default;
  MatchingEngine& operator=(MatchingEngine&&) noexcept = default;
  ~MatchingEngine() = default;

  /// Processes one command and APPENDS its events to `events` (existing
  /// contents are kept). Exceptions:
  ///   std::bad_alloc / std::length_error from reserving event capacity, and
  ///   std::overflow_error if the seq counter would wrap: nothing changed.
  ///   std::bad_alloc while resting a remainder: see the file comment.
  void process(const Command& command, std::vector<Event>& events);
  void process(const NewOrder& order, std::vector<Event>& events);
  void process(const CancelOrder& cancel, std::vector<Event>& events);

  /// Complete state for MatchingEngine::restore (DESIGN.md §6.5), in canonical
  /// order. O(n).
  [[nodiscard]] EngineSnapshot snapshot() const;

  /// An engine whose observable behaviour from now on is identical to that of
  /// the engine `snapshot` was taken from: same book (incl. priority seqs), same
  /// last_seq, same config. Throws std::invalid_argument if
  /// find_snapshot_violation(snapshot) reports a problem. O(n log n).
  [[nodiscard]] static MatchingEngine restore(const EngineSnapshot& snapshot);

  [[nodiscard]] const OrderBook& book() const noexcept { return book_; }
  [[nodiscard]] const EngineConfig& config() const noexcept { return book_.config(); }

  /// seq of the most recent event; 0 before the first event.
  [[nodiscard]] SeqNum last_seq() const noexcept { return last_seq_; }

  /// Book invariants (OrderBook::find_invariant_violation) plus: every resting
  /// order's seq is <= last_seq(). Returns the first violation or nullopt.
  [[nodiscard]] std::optional<std::string> find_invariant_violation() const;

 private:
  friend struct MatchingEngineTestAccess;  // tests only: seq exhaustion (D-015 pattern)

  /// Read-only dry run of matching `order` against the current book.
  struct MatchPlan {
    Quantity fill_quantity{0};      ///< total quantity the order would trade
    std::size_t trades{0};          ///< number of TradeExecuted events
    std::size_t makers_filled{0};   ///< makers that would be completely filled
  };

  [[nodiscard]] MatchPlan plan_match(const NewOrder& order) const;

  /// First seq of a block of `count` consecutive seqs after last_seq_.
  /// Throws std::overflow_error if the block would wrap. Does not modify.
  [[nodiscard]] SeqNum first_seq_of_block(std::size_t count) const;

  void reject(CommandType command, OrderId id, RejectReason reason, std::vector<Event>& events);

  OrderBook book_;
  SeqNum last_seq_{0};
};

}  // namespace lob
