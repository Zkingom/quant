#pragma once

// Allocation-failure injection for exception-safety tests.
//
// Linking support/alloc_fault.cpp into a test executable replaces the global
// operator new/delete of THAT executable (only the *_alloc_tests binaries
// do this). Injection is disarmed by default; a FaultWindow arms it around
// exactly one operation and fails at most one allocation, so GoogleTest and
// the test's own bookkeeping never see an injected failure.

namespace lob::test::alloc {

void arm(long fail_after);       // let `fail_after` allocations through, fail the next
void disarm() noexcept;
long allocations() noexcept;     // allocations observed since the last arm()
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

}  // namespace lob::test::alloc
