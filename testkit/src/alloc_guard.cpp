#include "lob/testkit/alloc_guard.hpp"

#include <atomic>
#include <cstdlib>
#include <new>

namespace {

thread_local bool tl_armed = false;
std::atomic<long> g_violations{0};
std::atomic<long long> g_violation_bytes{0};
std::atomic<long> g_total{0};

void* allocate(std::size_t size, std::size_t align) {
  g_total.fetch_add(1, std::memory_order_relaxed);
  if (tl_armed) {
    g_violations.fetch_add(1, std::memory_order_relaxed);
    g_violation_bytes.fetch_add(static_cast<long long>(size), std::memory_order_relaxed);
  }
  if (size == 0) {
    size = 1;
  }
  void* p = nullptr;
  if (align <= alignof(std::max_align_t)) {
    p = std::malloc(size);
  } else {
    const std::size_t rounded = (size + align - 1) / align * align;  // aligned_alloc precondition
    p = std::aligned_alloc(align, rounded);
  }
  if (p == nullptr) {
    throw std::bad_alloc();
  }
  return p;
}

void* allocate_nothrow(std::size_t size, std::size_t align) noexcept {
  try {
    return allocate(size, align);
  } catch (const std::bad_alloc&) {
    return nullptr;
  }
}

constexpr std::size_t kDefault = alignof(std::max_align_t);

}  // namespace

namespace lob::testkit::alloc_guard {

void arm_this_thread() noexcept { tl_armed = true; }
void disarm_this_thread() noexcept { tl_armed = false; }
long violations() noexcept { return g_violations.load(std::memory_order_relaxed); }
long long violation_bytes() noexcept { return g_violation_bytes.load(std::memory_order_relaxed); }
long total_allocations() noexcept { return g_total.load(std::memory_order_relaxed); }
void reset() noexcept {
  g_violations.store(0, std::memory_order_relaxed);
  g_violation_bytes.store(0, std::memory_order_relaxed);
  g_total.store(0, std::memory_order_relaxed);
}

}  // namespace lob::testkit::alloc_guard

// Every plain, nothrow and aligned form is replaced as one matched set
// (malloc/aligned_alloc + free), for the same ASan pairing reason as
// alloc_fault.cpp (D-016); aligned forms are included here because the
// SPSC queue and the pipeline use over-aligned types.
void* operator new(std::size_t n) { return allocate(n, kDefault); }
void* operator new[](std::size_t n) { return allocate(n, kDefault); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return allocate_nothrow(n, kDefault); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return allocate_nothrow(n, kDefault); }
void* operator new(std::size_t n, std::align_val_t a) { return allocate(n, static_cast<std::size_t>(a)); }
void* operator new[](std::size_t n, std::align_val_t a) { return allocate(n, static_cast<std::size_t>(a)); }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
  return allocate_nothrow(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
  return allocate_nothrow(n, static_cast<std::size_t>(a));
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }
