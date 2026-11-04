#pragma once

// Bounded lock-free single-producer / single-consumer ring buffer (L3' 1a).
//
// Exactly one thread may call try_push() and exactly one (possibly different)
// thread may call try_pop(); the pairing is a precondition, not checked.
// Elements are trivially copyable values: a slot is plain storage that the
// producer overwrites and the consumer copies out, so no constructor or
// destructor ever runs concurrently with the other side.
//
// Layout (DESIGN §7.1): the producer-owned index (tail_) and the
// consumer-owned index (head_) live on separate cache lines, each together
// with that side's private cached copy of the *other* index. A side only
// touches the other side's line when its cached copy says "full" (producer)
// or "empty" (consumer), so in steady state each line is written by one core
// and read by the other only on those refreshes.
//
// Indices are free-running 64-bit counters (never wrapped); slot = index &
// mask. 2^64 operations cannot overflow in practice, and full/empty are then
// simply tail - head == capacity / tail == head, with no wasted slot.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace lob::concurrent {

/// Assumed destructive-interference size. std::hardware_destructive_interference_size
/// is not used: GCC warns that its value may differ between translation units
/// (-Winterference-size), and every x86-64 and most AArch64 parts use 64 bytes.
inline constexpr std::size_t kCacheLine = 64;

template <typename T>
class SpscQueue {
  static_assert(std::is_trivially_copyable_v<T>, "SpscQueue slots are copied bytewise");
  static_assert(std::is_default_constructible_v<T>, "slots are value-initialised once");

 public:
  /// capacity must be a power of two >= 1. Allocates the ring once; no
  /// operation after construction allocates.
  explicit SpscQueue(std::size_t capacity)
      : mask_(checked_mask(capacity)), buffer_(std::make_unique<T[]>(capacity)) {}

  SpscQueue(const SpscQueue&) = delete;
  SpscQueue& operator=(const SpscQueue&) = delete;
  SpscQueue(SpscQueue&&) = delete;
  SpscQueue& operator=(SpscQueue&&) = delete;
  ~SpscQueue() = default;

  /// Producer only. Returns false (and leaves the queue unchanged) when full.
  [[nodiscard]] bool try_push(const T& value) noexcept {
    // relaxed: tail_ is written only by this (the producer) thread, so this
    // thread always observes its own latest store; no synchronisation needed.
    const std::uint64_t tail = producer_.tail.load(std::memory_order_relaxed);
    if (tail - producer_.cached_head == capacity()) {
      // acquire: pairs with the consumer's release store of head_ in
      // try_pop(). Everything the consumer did before releasing a slot -- in
      // particular copying the old value out of it -- happens-before our
      // overwrite below, so we never clobber an element still being read.
      producer_.cached_head = consumer_.head.load(std::memory_order_acquire);
      if (tail - producer_.cached_head == capacity()) {
        return false;
      }
    }
    buffer_[static_cast<std::size_t>(tail) & mask_] = value;
    // release: publishes the slot write above. A consumer that reads this
    // tail value with acquire is guaranteed to see the complete element.
    producer_.tail.store(tail + 1, std::memory_order_release);
    return true;
  }

  /// Consumer only. Returns false (and leaves `out` unchanged) when empty.
  [[nodiscard]] bool try_pop(T& out) noexcept {
    // relaxed: head_ is written only by this (the consumer) thread.
    const std::uint64_t head = consumer_.head.load(std::memory_order_relaxed);
    if (head == consumer_.cached_tail) {
      // acquire: pairs with the producer's release store of tail_ in
      // try_push(); makes the producer's slot write visible before we read it.
      consumer_.cached_tail = producer_.tail.load(std::memory_order_acquire);
      if (head == consumer_.cached_tail) {
        return false;
      }
    }
    out = buffer_[static_cast<std::size_t>(head) & mask_];
    // release: our read of the slot happens-before the producer reuses it
    // (the producer acquires head_ before overwriting; see try_push()).
    consumer_.head.store(head + 1, std::memory_order_release);
    return true;
  }

  [[nodiscard]] std::size_t capacity() const noexcept { return mask_ + 1; }

  /// Number of elements at some instant between the call and its return.
  /// Exact only when neither side is running concurrently (tests, shutdown).
  /// Both loads are acquire so that the result is never negative from the
  /// caller's point of view: head is read first, and tail >= head always.
  [[nodiscard]] std::size_t size_approx() const noexcept {
    const std::uint64_t head = consumer_.head.load(std::memory_order_acquire);
    const std::uint64_t tail = producer_.tail.load(std::memory_order_acquire);
    return static_cast<std::size_t>(tail - head);
  }

 private:
  static std::size_t checked_mask(std::size_t capacity) {
    if (capacity == 0 || (capacity & (capacity - 1)) != 0) {
      throw std::invalid_argument("SpscQueue capacity must be a power of two >= 1");
    }
    return capacity - 1;
  }

  // Why seq_cst is never needed: the only cross-thread facts are "slot i is
  // full" (published by tail) and "slot i is free" (published by head). Each
  // is a single-writer flag read by a single reader -- a pure message-passing
  // pattern that release/acquire fully orders. No decision depends on the
  // relative order of the two indices as seen by a third thread, which is
  // the only thing seq_cst would add.

  struct alignas(kCacheLine) ProducerSide {
    std::atomic<std::uint64_t> tail{0};
    std::uint64_t cached_head{0};  // producer-private snapshot of head
  };
  struct alignas(kCacheLine) ConsumerSide {
    std::atomic<std::uint64_t> head{0};
    std::uint64_t cached_tail{0};  // consumer-private snapshot of tail
  };
  static_assert(sizeof(ProducerSide) == kCacheLine && sizeof(ConsumerSide) == kCacheLine);
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

  ProducerSide producer_;
  ConsumerSide consumer_;
  // Read-only after construction: shared by both sides without contention.
  alignas(kCacheLine) const std::size_t mask_;
  const std::unique_ptr<T[]> buffer_;
};

}  // namespace lob::concurrent
