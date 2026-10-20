#pragma once

// Differential runner: MatchingEngine vs. ReferenceEngine on a generated
// workload, with the EventAuditor on top.
//
// Every command: complete event vectors must be equal (engine vs. reference)
// and the auditor must accept them (A1–A5 on the auditor's reconstruction).
// Every `full_check_every` commands and at the end: both books equal order
// by order (incl. seq), auditor.matches() against the engine book (A5/A6),
// engine invariants (M2 + I2), and state metrics are sampled.
//
// Optional fault injection (needs a FaultInjector, i.e. a binary linked with
// lob_testkit_alloc_fault): on a fraction of commands one allocation is made
// to fail. The outcome must be PREPARE (nothing changed; the reference skips
// the command) or RESIDUAL (the reference's events with the final RESTED
// replaced by CANCELLED{INSUFFICIENT_RESOURCES}; the reference then drops
// that order). The run then CONTINUES on the post-failure state.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "lob/core/reject_reason.hpp"
#include "lob/testkit/event_auditor.hpp"
#include "lob/testkit/workload.hpp"

namespace lob::testkit {

/// Abstract hook so the runner does not depend on the global operator new
/// replacement; see alloc_fault.hpp for the implementation.
class FaultInjector {
 public:
  virtual ~FaultInjector() = default;
  virtual void arm(long fail_after) = 0;
  virtual void disarm() noexcept = 0;
  [[nodiscard]] virtual bool injected() const noexcept = 0;
};

struct Coverage {
  // Same meaning as the M3 differential counters.
  long commands = 0;
  long accepted = 0;
  long trades = 0;
  long cancels = 0;
  long multi_maker_commands = 0;
  long multi_level_commands = 0;
  long taker_fully_filled = 0;
  long taker_partial_then_rested = 0;
  long rested_without_trade = 0;
  long maker_partially_filled = 0;
  long accepted_while_full = 0;
  long reused_dead_id = 0;
  long rejects[8] = {};  // indexed by RejectReason
  std::size_t max_orders = 0;

  // M4 state metrics.
  long max_trades_in_command = 0;
  long max_levels_swept = 0;          // distinct prices traded in one command
  long side_emptied = 0;              // commands that traded the opposite side empty
  long band_edge_accepts = 0;         // accepted at min_price or max_price
  long band_edge_trades = 0;
  std::size_t max_levels_per_side = 0;  // sampled at full checks (full depth)
  std::size_t max_level_orders = 0;     // longest FIFO queue: best levels after every
                                        // accepted order + all levels at full checks
  Quantity max_level_quantity = 0;      // largest level total, sampled likewise
  Int128 accepted_quantity = 0;         // cumulative (auditor), may exceed int64
  long full_checks = 0;

  // Fault injection.
  long faults_prepare = 0;
  long faults_residual = 0;
  long faults_armed_without_effect = 0;  // armed, but the command allocated less

  void merge(const Coverage& other);
  [[nodiscard]] long reject_count(RejectReason r) const { return rejects[static_cast<int>(r)]; }
  [[nodiscard]] std::string summary() const;  // multi-line, "[coverage] ..." prefixed
};

struct RunOptions {
  std::uint64_t seed = 1;
  long commands = 1'000;
  long full_check_every = 1;
  int fault_per_mille = 0;           // per-command probability of arming a failure
  FaultInjector* injector = nullptr;  // required when fault_per_mille > 0
};

struct RunReport {
  bool ok = true;
  long failed_step = -1;   // step at which the violation was detected
  std::string failure;     // description, including the command and both event streams
  Coverage coverage;
  double seconds = 0;
};

RunReport run_differential(const WorkloadProfile& profile, const RunOptions& options);

/// The profile's boundary targets that `coverage` does not reach (empty =
/// all reached). Meaningful only for runs of >= profile.target_commands
/// commands per seed.
std::vector<std::string> unmet_targets(const WorkloadProfile& profile, const Coverage& coverage);

}  // namespace lob::testkit
