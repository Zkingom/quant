#pragma once

// M5: the complete engine state needed to continue processing exactly where
// a previous engine stopped (DESIGN.md §6.5): config, the last event seq and
// every resting order with its priority seq. Plain data, no I/O.
//
// Canonical order (required by find_snapshot_violation, produced by
// MatchingEngine::snapshot): all bids best price first, FIFO (ascending seq)
// within a price; then all asks best price first, FIFO within a price.
// Equal states therefore have equal snapshots, element by element.

#include <optional>
#include <string>
#include <vector>

#include "lob/core/config.hpp"
#include "lob/core/types.hpp"

namespace lob {

struct SnapshotOrder {
  OrderId id{kInvalidOrderId};
  Side side{Side::Buy};
  Price price{0};
  Quantity remaining{0};
  SeqNum seq{0};  ///< priority (the order's OrderAccepted seq)

  friend bool operator==(const SnapshotOrder&, const SnapshotOrder&) = default;
};

struct EngineSnapshot {
  EngineConfig config;
  SeqNum last_seq{0};  ///< seq of the last event emitted; the next event is last_seq + 1
  std::vector<SnapshotOrder> orders;

  friend bool operator==(const EngineSnapshot&, const EngineSnapshot&) = default;
};

/// Everything MatchingEngine::restore requires, checked up front so that a
/// corrupt or hand-edited snapshot is reported as data, not as a contract
/// violation deep inside the order book. Returns the first problem found:
/// invalid config; more orders than max_live_orders; an order with id 0,
/// invalid side, price outside the band, remaining outside
/// (0, max_order_quantity], or seq outside [1, last_seq]; duplicate ids or
/// seqs; non-canonical order; crossed or locked book.
[[nodiscard]] std::optional<std::string> find_snapshot_violation(const EngineSnapshot& snapshot);

}  // namespace lob
