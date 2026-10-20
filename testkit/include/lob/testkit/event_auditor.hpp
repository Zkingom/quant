#pragma once

// EventAuditor: checks the engine's event stream against the properties in
// DESIGN.md §3.6 (A1–A6), using only the commands and the events.
//
// It keeps its own reconstruction of the resting book (a flat vector, linear
// scans) and derives from it what every event must look like: which maker a
// trade must hit (price-time priority), at what price, for how much, and
// what each remaining quantity must be. It does not implement matching
// itself; it only checks that each event is the one the rules allow.
//
// Usage: for every processed command, call audit(command, events_of_command),
// then (as often as affordable) matches(engine.book()). Both return
// std::nullopt when the property holds, otherwise a description of the first
// violation. No GoogleTest dependency (reused by long-run tools).
//
// Cumulative totals are 128-bit: per-order quantities are bounded by the
// config (D-006), but sums over a long run with large quantities exceed
// int64 (M4 finding; an int64 total would be signed-overflow UB).

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "lob/book/order_book.hpp"
#include "lob/core/command.hpp"
#include "lob/core/config.hpp"
#include "lob/engine/events.hpp"

namespace lob::testkit {

__extension__ using Int128 = __int128;  // GCC/Clang; __extension__ silences -Wpedantic

std::string to_string(Int128 value);

class EventAuditor {
 public:
  using Violation = std::optional<std::string>;

  explicit EventAuditor(const EngineConfig& config) : config_(config) {}

  /// A1–A5 for the events produced by one command.
  [[nodiscard]] Violation audit(const Command& command, std::span<const Event> events);

  /// A6 (reconstruction == engine book, including seq) and A5 stated against
  /// the ENGINE's book: accepted = 2 * traded + resting in engine + cancelled.
  /// (The per-command check in audit() uses the auditor's own
  /// reconstruction, where it holds by construction once every event has
  /// been validated; this is the engine-facing statement.)
  [[nodiscard]] Violation matches(const OrderBook& book) const;

  [[nodiscard]] Int128 accepted_quantity() const { return accepted_; }
  [[nodiscard]] Int128 traded_quantity() const { return traded_; }
  [[nodiscard]] Int128 cancelled_quantity() const { return cancelled_; }
  [[nodiscard]] std::size_t resting_count() const { return resting_.size(); }

 private:
  struct Resting {
    OrderId id;
    Side side;
    Price price;
    Quantity original;  // 0 in comparison copies
    Quantity remaining;
    SeqNum seq;
  };

  Violation audit_new(const NewOrder& order);
  Violation audit_cancel(const CancelOrder& cancel);
  Violation audit_new_rejection(const NewOrder& order, const OrderRejected& rejected);
  Violation audit_trade(const NewOrder& order, const TradeExecuted& trade, Quantity& remaining);
  Violation check_seq(const Event& e);
  [[nodiscard]] Violation check_conservation() const;

  Resting* best_on(Side side);
  [[nodiscard]] std::vector<Resting> sorted(Side side) const;
  [[nodiscard]] const Resting* find(OrderId id) const;
  std::vector<Resting>::iterator position(OrderId id);
  [[nodiscard]] const Event* peek() const;
  const Event* take();

  EngineConfig config_;
  std::vector<Resting> resting_;
  SeqNum expected_seq_{1};
  Int128 accepted_{0};
  Int128 traded_{0};
  Int128 cancelled_{0};
  std::span<const Event> events_;
  std::size_t next_{0};
};

}  // namespace lob::testkit
