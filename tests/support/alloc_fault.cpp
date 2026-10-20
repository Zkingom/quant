#include "support/alloc_fault.hpp"

#include <cstddef>
#include <cstdlib>
#include <new>

namespace {

// The *_alloc_tests binaries are single-threaded: plain globals suffice.
bool g_armed = false;
long g_countdown = 0;    // allocations to let through before failing one
long g_allocations = 0;  // allocations observed while armed
long g_injected = 0;     // failures injected while armed

void* allocate(std::size_t size) {
  if (g_armed) {
    ++g_allocations;
    if (g_countdown == 0) {
      g_armed = false;  // fail exactly once per armed window
      ++g_injected;
      throw std::bad_alloc();
    }
    --g_countdown;
  }
  if (void* p = std::malloc(size == 0 ? 1 : size)) {
    return p;
  }
  throw std::bad_alloc();
}

}  // namespace

namespace lob::test::alloc {

void arm(long fail_after) {
  g_allocations = 0;
  g_injected = 0;
  g_countdown = fail_after;
  g_armed = true;
}
void disarm() noexcept { g_armed = false; }
long allocations() noexcept { return g_allocations; }
bool injected() noexcept { return g_injected > 0; }

}  // namespace lob::test::alloc

// Replaceable global allocation functions ([new.delete]). Every plain and
// nothrow form is replaced so that each allocation/deallocation pair goes
// through malloc/free: if only the plain forms were replaced, a nothrow new
// served by a sanitizer's own operator new (GoogleTest uses
// new(std::nothrow)) would be released by our free-based delete, which ASan
// reports as alloc-dealloc-mismatch. The nothrow forms take part in
// injection like the standard defaults do (they call the plain form).
// Aligned forms are left to the implementation as a matched set; nothing in
// these binaries over-aligns.
void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  try {
    return allocate(size);
  } catch (const std::bad_alloc&) {
    return nullptr;
  }
}
void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
  return operator new(size, tag);
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
