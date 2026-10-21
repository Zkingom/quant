#pragma once

// Allocation-failure injection for exception-safety tests.
//
// Linking the lob_testkit_alloc_fault object library into an executable
// replaces the global operator new/delete of THAT executable (only the
// *_alloc_tests binaries and lob_soak do this). Injection is disarmed by default; a FaultWindow arms it around
// exactly one operation and fails at most one allocation, so GoogleTest and
// the test's own bookkeeping never see an injected failure.

#include "lob/testkit/differential.hpp"

namespace lob::testkit::alloc {

void arm(long fail_after);       // let `fail_after` allocations through, fail the next
void disarm() noexcept;
long allocations() noexcept;     // allocations observed since the last arm()
long long allocated_bytes() noexcept;  // bytes those allocations requested
bool injected() noexcept;        // whether the armed window injected its failure

class FaultWindow {
 public:
  explicit FaultWindow(long fail_after) { arm(fail_after); }
  ~FaultWindow() { disarm(); }
  FaultWindow(const FaultWindow&) = delete;
  FaultWindow& operator=(const FaultWindow&) = delete;

  [[nodiscard]] long allocations() const noexcept { return alloc::allocations(); }
  [[nodiscard]] bool injected() const noexcept { return alloc::injected(); }
};

/// Adapter for run_differential(): arms/disarms the global replacement.
class Injector final : public FaultInjector {
 public:
  void arm(long fail_after) override { alloc::arm(fail_after); }
  void disarm() noexcept override { alloc::disarm(); }
  [[nodiscard]] bool injected() const noexcept override { return alloc::injected(); }
};

}  // namespace lob::testkit::alloc
