// L3' 1a: SpscQueue semantics, differential against std::queue, and
// two-thread stress (run under TSan by `scripts/check.sh --tsan`).

#include "lob/concurrent/spsc_queue.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>

#include "lob/testkit/rng.hpp"

namespace {

using lob::concurrent::SpscQueue;

// A multi-word payload: a torn or stale read shows up as a checksum mismatch.
struct Item {
  std::uint64_t seq;
  std::uint64_t a;
  std::uint64_t b;
  std::uint64_t check;
};

Item make_item(std::uint64_t seq) {
  const std::uint64_t a = seq * 0x9e3779b97f4a7c15ULL;
  const std::uint64_t b = ~seq ^ (a >> 7);
  return Item{seq, a, b, seq ^ a ^ b};
}

bool item_ok(const Item& it) { return (it.seq ^ it.a ^ it.b) == it.check; }

TEST(SpscQueue, RejectsCapacityThatIsNotAPowerOfTwo) {
  EXPECT_THROW(SpscQueue<int>{0}, std::invalid_argument);
  EXPECT_THROW(SpscQueue<int>{3}, std::invalid_argument);
  EXPECT_THROW(SpscQueue<int>{1000}, std::invalid_argument);
  EXPECT_NO_THROW(SpscQueue<int>{1});
  EXPECT_NO_THROW(SpscQueue<int>{1024});
}

TEST(SpscQueue, PopFromEmptyFailsAndLeavesOutputUnchanged) {
  SpscQueue<int> q{4};
  int out = 42;
  EXPECT_FALSE(q.try_pop(out));
  EXPECT_EQ(out, 42);
  EXPECT_EQ(q.size_approx(), 0U);
}

TEST(SpscQueue, FillsExactlyToCapacityThenRejects) {
  SpscQueue<int> q{8};
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(q.try_push(i)) << i;
  }
  EXPECT_FALSE(q.try_push(99));
  EXPECT_EQ(q.size_approx(), 8U);
  for (int i = 0; i < 8; ++i) {
    int out = -1;
    ASSERT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, i);
  }
  int out = -1;
  EXPECT_FALSE(q.try_pop(out));
}

TEST(SpscQueue, CapacityOneAlternates) {
  SpscQueue<int> q{1};
  for (int i = 0; i < 100; ++i) {
    ASSERT_TRUE(q.try_push(i));
    ASSERT_FALSE(q.try_push(i + 1000));
    int out = -1;
    ASSERT_TRUE(q.try_pop(out));
    ASSERT_EQ(out, i);
    ASSERT_FALSE(q.try_pop(out));
  }
}

TEST(SpscQueue, FifoAcrossManyWrapArounds) {
  SpscQueue<std::uint64_t> q{4};
  std::uint64_t next_in = 0;
  std::uint64_t next_out = 0;
  for (int round = 0; round < 1000; ++round) {
    const int pushes = 1 + round % 4;
    for (int i = 0; i < pushes; ++i) {
      ASSERT_TRUE(q.try_push(next_in++));
    }
    for (int i = 0; i < pushes; ++i) {
      std::uint64_t out = 0;
      ASSERT_TRUE(q.try_pop(out));
      ASSERT_EQ(out, next_out++);
    }
  }
  EXPECT_EQ(next_in, next_out);
}

// Single-threaded differential: a random push/pop script applied to the
// SPSC queue and to a capacity-bounded std::queue model must agree on every
// return value and every popped element.
TEST(SpscQueue, DifferentialAgainstStdQueue) {
  for (const std::size_t capacity : {1U, 2U, 4U, 16U, 256U}) {
    for (std::uint64_t seed = 1; seed <= 5; ++seed) {
      lob::testkit::Rng rng{seed * 7919 + capacity};
      SpscQueue<Item> q{capacity};
      std::queue<Item> model;
      std::uint64_t seq = 0;
      for (int step = 0; step < 20000; ++step) {
        // Bias the mix so that both "full" and "empty" are visited often.
        const bool push = rng.uniform(0, 99) < (step / 2000 % 2 == 0 ? 60 : 40);
        if (push) {
          const Item it = make_item(seq++);
          const bool model_ok = model.size() < capacity;
          ASSERT_EQ(q.try_push(it), model_ok) << "cap " << capacity << " step " << step;
          if (model_ok) {
            model.push(it);
          }
        } else {
          Item out{};
          const bool got = q.try_pop(out);
          ASSERT_EQ(got, !model.empty()) << "cap " << capacity << " step " << step;
          if (got) {
            ASSERT_EQ(out.seq, model.front().seq);
            ASSERT_TRUE(item_ok(out));
            model.pop();
          }
        }
        ASSERT_EQ(q.size_approx(), model.size());
      }
    }
  }
}

// Two threads. The producer pushes a deterministic sequence (spinning on
// full); the consumer pops (spinning on empty) and records it. The recorded
// stream must equal what a std::queue would deliver: the same elements, in
// order, intact.
void run_two_thread(std::size_t capacity, std::uint64_t count) {
  SpscQueue<Item> q{capacity};
  std::vector<Item> received;
  received.reserve(count);

  std::thread consumer([&] {
    Item out{};
    while (received.size() < count) {
      if (q.try_pop(out)) {
        received.push_back(out);
      } else {
        std::this_thread::yield();
      }
    }
  });
  std::uint64_t full_spins = 0;
  for (std::uint64_t i = 0; i < count; ++i) {
    const Item it = make_item(i);
    while (!q.try_push(it)) {
      ++full_spins;
      std::this_thread::yield();
    }
  }
  consumer.join();

  std::queue<Item> model;
  for (std::uint64_t i = 0; i < count; ++i) {
    model.push(make_item(i));
  }
  ASSERT_EQ(received.size(), model.size());
  for (std::uint64_t i = 0; i < count; ++i) {
    ASSERT_TRUE(item_ok(received[i])) << "torn element at " << i;
    ASSERT_EQ(received[i].seq, model.front().seq) << "order differs at " << i;
    model.pop();
  }
  Item out{};
  EXPECT_FALSE(q.try_pop(out));
  // With small capacities the producer must have hit "full" at least once,
  // otherwise the refresh-on-full path was not exercised.
  if (capacity <= 2) {
    EXPECT_GT(full_spins, 0U);
  }
}

#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
constexpr std::uint64_t kStressCount = 200'000;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
constexpr std::uint64_t kStressCount = 200'000;
#else
constexpr std::uint64_t kStressCount = 2'000'000;
#endif
#else
constexpr std::uint64_t kStressCount = 2'000'000;
#endif

TEST(SpscQueueStress, TwoThreadsCapacity1) { run_two_thread(1, kStressCount / 10); }
TEST(SpscQueueStress, TwoThreadsCapacity2) { run_two_thread(2, kStressCount / 4); }
TEST(SpscQueueStress, TwoThreadsCapacity64) { run_two_thread(64, kStressCount); }
TEST(SpscQueueStress, TwoThreadsCapacity65536) { run_two_thread(65536, kStressCount); }

// Producer and consumer bursts of random length: exercises the cached-index
// refresh paths (full -> refresh head, empty -> refresh tail) at varying
// fill levels instead of the steady near-empty state of the test above.
TEST(SpscQueueStress, RandomBurstsMatchModel) {
  constexpr std::size_t kCap = 16;
  const std::uint64_t count = kStressCount / 4;
  SpscQueue<Item> q{kCap};
  std::vector<std::uint64_t> received;
  received.reserve(count);
  std::thread consumer([&] {
    lob::testkit::Rng rng{2024};
    Item out{};
    while (received.size() < count) {
      const auto burst = rng.uniform(1, 40);
      for (std::int64_t i = 0; i < burst && received.size() < count; ++i) {
        if (q.try_pop(out)) {
          if (!item_ok(out)) {
            received.push_back(~0ULL);  // recorded as a failure below
          } else {
            received.push_back(out.seq);
          }
        }
      }
      std::this_thread::yield();
    }
  });
  lob::testkit::Rng rng{4048};
  std::uint64_t next = 0;
  while (next < count) {
    const auto burst = rng.uniform(1, 40);
    for (std::int64_t i = 0; i < burst && next < count; ++i) {
      if (q.try_push(make_item(next))) {
        ++next;
      }
    }
    std::this_thread::yield();
  }
  consumer.join();
  ASSERT_EQ(received.size(), count);
  for (std::uint64_t i = 0; i < count; ++i) {
    ASSERT_EQ(received[i], i);
  }
}

}  // namespace
