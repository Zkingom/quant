#pragma once

// Zero-allocation guard (L3' stage 2). Linking lob_testkit_alloc_guard
// replaces the global operator new/delete of that executable (only
// lob_pipeline_alloc_tests). A thread that arms the guard counts every
// allocation it makes as a violation until it disarms; other threads
// (GoogleTest, setup code) are unaffected. The test fails if a hot loop
// records any violation.

#include <cstddef>

namespace lob::testkit::alloc_guard {

void arm_this_thread() noexcept;
void disarm_this_thread() noexcept;
/// Allocations made by armed threads since the last reset().
long violations() noexcept;
/// Bytes requested by those allocations.
long long violation_bytes() noexcept;
/// All allocations observed (armed or not) since the last reset().
long total_allocations() noexcept;
void reset() noexcept;

}  // namespace lob::testkit::alloc_guard
