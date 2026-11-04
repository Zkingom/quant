// L3' 1c: md::Book update semantics, invariants, and differential testing
// against the naive testkit::ReferenceMdBook.

#include "lob/md/book.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "lob/testkit/md_reference_book.hpp"
#include "lob/testkit/rng.hpp"

namespace {

using lob::md::Book;
using lob::md::BookConfig;
using lob::md::BookStatus;
using lob::md::Side;
constexpr Side B = Side::Buy;
constexpr Side S = Side::Sell;

#define EXPECT_CONSISTENT(book) EXPECT_EQ((book).find_invariant_violation(), std::nullopt)

TEST(MdBook, AddBuildsLevelsBestFirst) {
  Book b{BookConfig{16, 8}};
  ASSERT_EQ(b.add(1, B, 100, 9900), BookStatus::Applied);
  ASSERT_EQ(b.add(2, B, 50, 10000), BookStatus::Applied);
  ASSERT_EQ(b.add(3, B, 25, 10000), BookStatus::Applied);
  ASSERT_EQ(b.add(4, S, 10, 10100), BookStatus::Applied);
  ASSERT_EQ(b.add(5, S, 20, 10200), BookStatus::Applied);
  EXPECT_CONSISTENT(b);
  EXPECT_EQ(b.best(B)->price, 10000U);
  EXPECT_EQ(b.best(B)->shares, 75U);
  EXPECT_EQ(b.best(B)->orders, 2U);
  EXPECT_EQ(b.best(S)->price, 10100U);
  EXPECT_EQ(b.level_count(B), 2U);
  EXPECT_EQ(b.level_at(B, 1).price, 9900U);
  EXPECT_EQ(b.level_at(S, 1).price, 10200U);
  EXPECT_EQ(b.order_count(), 5U);
}

TEST(MdBook, NeverLocksOrCrosses) {
  Book b{BookConfig{16, 8}};
  ASSERT_EQ(b.add(1, B, 100, 10000), BookStatus::Applied);
  ASSERT_EQ(b.add(2, S, 100, 10100), BookStatus::Applied);
  const auto h = b.state_hash();
  EXPECT_EQ(b.add(3, S, 1, 10000), BookStatus::WouldLock);
  EXPECT_EQ(b.add(4, S, 1, 9999), BookStatus::WouldCross);
  EXPECT_EQ(b.add(5, B, 1, 10100), BookStatus::WouldLock);
  EXPECT_EQ(b.add(6, B, 1, 20000), BookStatus::WouldCross);
  EXPECT_EQ(b.replace(1, 7, 100, 10100), BookStatus::WouldLock);
  EXPECT_EQ(b.replace(1, 7, 100, 10200), BookStatus::WouldCross);
  EXPECT_EQ(b.state_hash(), h) << "rejections must not change the book";
  EXPECT_CONSISTENT(b);
}

TEST(MdBook, NonStrictModeAllowsLockingAndCrossingAndReportsIt) {
  Book b{BookConfig{16, 8}};
  b.set_strict(false);
  ASSERT_EQ(b.add(1, B, 100, 10000), BookStatus::Applied);
  ASSERT_EQ(b.add(2, S, 100, 10100), BookStatus::Applied);
  EXPECT_EQ(b.cross_state(), lob::md::CrossState::None);
  ASSERT_EQ(b.add(3, S, 50, 10000), BookStatus::Applied);  // locks
  EXPECT_EQ(b.cross_state(), lob::md::CrossState::Locked);
  ASSERT_EQ(b.add(4, B, 50, 10200), BookStatus::Applied);  // crosses
  EXPECT_EQ(b.cross_state(), lob::md::CrossState::Crossed);
  ASSERT_EQ(b.replace(2, 5, 100, 9000), BookStatus::Applied);  // replace through the bid
  EXPECT_CONSISTENT(b) << "a crossed book is consistent in non-strict mode";
  // Other checks still apply.
  EXPECT_EQ(b.add(6, B, 0, 10000), BookStatus::ZeroShares);
  EXPECT_EQ(b.execute(4, 51), BookStatus::Overfill);
  // Strict mode on a crossed book: the invariant check reports it, adds that cross are rejected.
  b.set_strict(true);
  EXPECT_NE(b.find_invariant_violation(), std::nullopt);
  EXPECT_EQ(b.add(7, S, 1, 9000), BookStatus::WouldCross);
  // Uncross (the halt cross executes the crossing orders), then strict holds again.
  ASSERT_EQ(b.execute(4, 50), BookStatus::Applied);
  ASSERT_EQ(b.remove(3), BookStatus::Applied);
  ASSERT_EQ(b.remove(5), BookStatus::Applied);
  EXPECT_EQ(b.cross_state(), lob::md::CrossState::None);
  EXPECT_CONSISTENT(b);
}

TEST(MdBook, ExecutionsCanHitAnyOrderNotOnlyTheQueueHead) {
  Book b{BookConfig{16, 8}};
  ASSERT_EQ(b.add(1, S, 100, 10100), BookStatus::Applied);
  ASSERT_EQ(b.add(2, S, 200, 10100), BookStatus::Applied);
  ASSERT_EQ(b.execute(2, 50), BookStatus::Applied);  // not the head
  EXPECT_EQ(b.find(2)->shares, 150U);
  EXPECT_EQ(b.level(S, 10100)->shares, 250U);
  ASSERT_EQ(b.execute(2, 150), BookStatus::Applied);
  EXPECT_FALSE(b.find(2).has_value());
  EXPECT_EQ(b.level(S, 10100)->orders, 1U);
  EXPECT_CONSISTENT(b);
}

TEST(MdBook, PartialCancelThenDeleteRemovesTheLevel) {
  Book b{BookConfig{16, 8}};
  ASSERT_EQ(b.add(1, B, 100, 10000), BookStatus::Applied);
  ASSERT_EQ(b.cancel(1, 30), BookStatus::Applied);
  EXPECT_EQ(b.find(1)->shares, 70U);
  EXPECT_EQ(b.last_change().level_shares, 70U);
  ASSERT_EQ(b.remove(1), BookStatus::Applied);
  EXPECT_EQ(b.level_count(B), 0U);
  EXPECT_EQ(b.last_change().level_shares, 0U);
  EXPECT_CONSISTENT(b);
}

TEST(MdBook, CancelOfAllRemainingSharesRemovesTheOrder) {
  Book b{BookConfig{16, 8}};
  ASSERT_EQ(b.add(1, B, 100, 10000), BookStatus::Applied);
  ASSERT_EQ(b.cancel(1, 100), BookStatus::Applied);
  EXPECT_EQ(b.order_count(), 0U);
  EXPECT_CONSISTENT(b);
}

TEST(MdBook, QuantitiesNeverGoNegative) {
  Book b{BookConfig{16, 8}};
  ASSERT_EQ(b.add(1, B, 100, 10000), BookStatus::Applied);
  EXPECT_EQ(b.execute(1, 101), BookStatus::Overfill);
  EXPECT_EQ(b.cancel(1, 101), BookStatus::Overfill);
  EXPECT_EQ(b.execute(1, 0), BookStatus::ZeroShares);
  EXPECT_EQ(b.cancel(1, 0), BookStatus::ZeroShares);
  EXPECT_EQ(b.add(2, B, 0, 10000), BookStatus::ZeroShares);
  EXPECT_EQ(b.add(3, B, 1, 0), BookStatus::ZeroPrice);
  EXPECT_EQ(b.find(1)->shares, 100U);
  EXPECT_CONSISTENT(b);
}

TEST(MdBook, UnknownAndDuplicateRefs) {
  Book b{BookConfig{16, 8}};
  ASSERT_EQ(b.add(1, B, 100, 10000), BookStatus::Applied);
  EXPECT_EQ(b.add(1, S, 100, 10500), BookStatus::DuplicateOrderRef);
  EXPECT_EQ(b.execute(9, 1), BookStatus::UnknownOrderRef);
  EXPECT_EQ(b.cancel(9, 1), BookStatus::UnknownOrderRef);
  EXPECT_EQ(b.remove(9), BookStatus::UnknownOrderRef);
  EXPECT_EQ(b.replace(9, 10, 1, 10000), BookStatus::UnknownOrderRef);
  ASSERT_EQ(b.add(2, B, 100, 9000), BookStatus::Applied);
  EXPECT_EQ(b.replace(1, 2, 1, 10000), BookStatus::DuplicateOrderRef);
  // A ref may be reused after the order is gone.
  ASSERT_EQ(b.remove(1), BookStatus::Applied);
  EXPECT_EQ(b.add(1, S, 5, 10500), BookStatus::Applied);
  EXPECT_CONSISTENT(b);
}

TEST(MdBook, ReplaceLosesPriorityAndKeepsSide) {
  Book b{BookConfig{16, 8}};
  ASSERT_EQ(b.add(1, B, 100, 10000), BookStatus::Applied);
  ASSERT_EQ(b.add(2, B, 100, 10000), BookStatus::Applied);
  EXPECT_EQ(b.shares_ahead(2), 100U);
  ASSERT_EQ(b.replace(1, 3, 100, 10000), BookStatus::Applied);  // same price, new ref
  EXPECT_FALSE(b.find(1).has_value());
  EXPECT_EQ(b.find(3)->side, B);
  EXPECT_EQ(b.shares_ahead(2), 0U);
  EXPECT_EQ(b.shares_ahead(3), 100U);
  ASSERT_EQ(b.replace(3, 3, 40, 9990), BookStatus::Applied);  // same ref allowed
  EXPECT_EQ(b.find(3)->price, 9990U);
  EXPECT_EQ(b.level_count(B), 2U);
  EXPECT_CONSISTENT(b);
}

TEST(MdBook, CapacityIsReportedNotReallocated) {
  Book b{BookConfig{3, 2}};
  ASSERT_EQ(b.add(1, B, 1, 100), BookStatus::Applied);
  ASSERT_EQ(b.add(2, B, 1, 101), BookStatus::Applied);
  EXPECT_EQ(b.add(3, B, 1, 102), BookStatus::LevelCapacity);
  ASSERT_EQ(b.add(3, B, 1, 101), BookStatus::Applied);
  EXPECT_EQ(b.add(4, S, 1, 200), BookStatus::OrderCapacity);
  // Replacing the only order of a level by one at a new price frees the level slot.
  ASSERT_EQ(b.replace(1, 5, 1, 99), BookStatus::Applied);
  EXPECT_EQ(b.level_count(B), 2U);
  EXPECT_CONSISTENT(b);
}

TEST(MdBook, ClearEmptiesAndTheBookIsReusable) {
  Book b{BookConfig{4, 4}};
  ASSERT_EQ(b.add(1, B, 1, 100), BookStatus::Applied);
  ASSERT_EQ(b.add(2, S, 1, 200), BookStatus::Applied);
  b.clear();
  EXPECT_EQ(b.order_count(), 0U);
  const Book fresh{BookConfig{4, 4}};
  EXPECT_EQ(b.state_hash(), fresh.state_hash());
  EXPECT_CONSISTENT(b);
  for (std::uint64_t r = 1; r <= 4; ++r) {
    EXPECT_EQ(b.add(r, B, 1, static_cast<std::uint32_t>(100 + r)), BookStatus::Applied);
  }
}

TEST(MdBook, StateHashDependsOnQueueOrderNotStorage) {
  Book a{BookConfig{16, 8}};
  Book b{BookConfig{64, 32}};  // different storage layout
  for (Book* x : {&a, &b}) {
    ASSERT_EQ(x->add(5, B, 10, 100), BookStatus::Applied);
    ASSERT_EQ(x->add(6, B, 20, 100), BookStatus::Applied);
  }
  EXPECT_EQ(a.state_hash(), b.state_hash());
  Book c{BookConfig{16, 8}};
  ASSERT_EQ(c.add(6, B, 20, 100), BookStatus::Applied);
  ASSERT_EQ(c.add(5, B, 10, 100), BookStatus::Applied);
  EXPECT_NE(a.state_hash(), c.state_hash()) << "same levels, different queue order";
}

TEST(MdBook, IndexSurvivesHeavyChurnWithCollidingRefs) {
  // Many refs, constant removal: exercises backward-shift deletion.
  Book b{BookConfig{256, 256}};
  lob::testkit::Rng rng{11};
  std::vector<std::uint64_t> live;
  for (int i = 0; i < 20000; ++i) {
    if (live.size() < 200 && (live.empty() || rng.percent(55))) {
      const std::uint64_t ref = static_cast<std::uint64_t>(rng.uniform(1, 1 << 30)) << 12;
      if (b.add(ref, B, 1, static_cast<std::uint32_t>(rng.uniform(1, 200))) == BookStatus::Applied) {
        live.push_back(ref);
      }
    } else {
      const auto k = static_cast<std::size_t>(rng.uniform(0, static_cast<std::int64_t>(live.size()) - 1));
      ASSERT_EQ(b.remove(live[k]), BookStatus::Applied);
      live[k] = live.back();
      live.pop_back();
    }
    if (i % 97 == 0) {
      ASSERT_EQ(b.find_invariant_violation(), std::nullopt) << i;
    }
  }
  for (const auto r : live) {
    EXPECT_TRUE(b.find(r).has_value());
  }
}

// ---------------------------------------------------------------- differential

struct DiffStats {
  long applied = 0;
  long rejected = 0;
};

void run_differential(std::uint64_t seed, std::size_t max_orders, std::size_t max_levels, int steps,
                      DiffStats& stats) {
  Book book{BookConfig{max_orders, max_levels}};
  lob::testkit::ReferenceMdBook ref{max_orders, max_levels};
  lob::testkit::Rng rng{seed};
  std::vector<std::uint64_t> refs;  // ever used (live or not)
  std::uint64_t next_ref = 1;
  const std::uint32_t mid = 10000;

  auto pick_ref = [&]() -> std::uint64_t {
    if (refs.empty() || rng.percent(8)) return next_ref + 1000;  // unknown
    return refs[static_cast<std::size_t>(rng.uniform(0, static_cast<std::int64_t>(refs.size()) - 1))];
  };
  auto pick_shares = [&](std::uint64_t r) -> std::uint32_t {
    const auto live = book.find(r);
    const int k = static_cast<int>(rng.uniform(0, 99));
    if (k < 4) return 0;
    if (live && k < 10) return live->shares + 1;  // overfill
    if (live && k < 40) return live->shares;      // exactly all
    if (live) return static_cast<std::uint32_t>(rng.uniform(1, live->shares));
    return static_cast<std::uint32_t>(rng.uniform(1, 500));
  };
  auto pick_price = [&](Side s) -> std::uint32_t {
    if (rng.percent(2)) return 0;
    // Mostly on the own side of mid, sometimes through it (lock / cross).
    const auto off = static_cast<std::uint32_t>(rng.uniform(0, 30));
    const bool through = rng.percent(10);
    const bool below = (s == Side::Buy) != through;
    return below ? mid - 1 - off : mid + off;
  };

  for (int step = 0; step < steps; ++step) {
    const int op = static_cast<int>(rng.uniform(0, 99));
    BookStatus got{};
    BookStatus want{};
    if (op < 40) {
      const Side s = rng.side() == lob::Side::Buy ? Side::Buy : Side::Sell;
      const std::uint64_t r = rng.percent(5) && !refs.empty() ? pick_ref() : next_ref++;
      const auto shares = rng.percent(3) ? 0U : static_cast<std::uint32_t>(rng.uniform(1, 500));
      const auto price = pick_price(s);
      got = book.add(r, s, shares, price);
      want = ref.add(r, s, shares, price);
      refs.push_back(r);
    } else if (op < 60) {
      const auto r = pick_ref();
      const auto sh = pick_shares(r);
      got = book.execute(r, sh);
      want = ref.execute(r, sh);
    } else if (op < 75) {
      const auto r = pick_ref();
      const auto sh = pick_shares(r);
      got = book.cancel(r, sh);
      want = ref.cancel(r, sh);
    } else if (op < 87) {
      const auto r = pick_ref();
      got = book.remove(r);
      want = ref.remove(r);
    } else {
      const auto r = pick_ref();
      const auto live = book.find(r);
      const Side s = live ? live->side : Side::Buy;
      const std::uint64_t nr = rng.percent(5) ? pick_ref() : (rng.percent(5) ? r : next_ref++);
      const auto shares = rng.percent(3) ? 0U : static_cast<std::uint32_t>(rng.uniform(1, 500));
      const auto price = rng.percent(30) && live ? live->price : pick_price(s);
      got = book.replace(r, nr, shares, price);
      want = ref.replace(r, nr, shares, price);
      refs.push_back(nr);
    }
    ASSERT_EQ(got, want) << "seed " << seed << " step " << step << ": " << lob::md::to_string(got)
                         << " vs " << lob::md::to_string(want);
    (got == BookStatus::Applied ? stats.applied : stats.rejected)++;
    ASSERT_EQ(book.order_count(), ref.order_count()) << "step " << step;
    // The reference recomputes everything by scanning; a full comparison on
    // every step is affordable only for small books.
    if (max_orders <= 16 || step % 10 == 0) {
      ASSERT_EQ(book.state_hash(), ref.state_hash()) << "seed " << seed << " step " << step;
    }
    ASSERT_EQ(book.find_invariant_violation(), std::nullopt) << "step " << step;
    for (const Side s : {Side::Buy, Side::Sell}) {
      const auto x = book.best(s);
      const auto y = ref.best(s);
      ASSERT_EQ(x.has_value(), y.has_value());
      if (x) {
        ASSERT_EQ(x->price, y->price);
        ASSERT_EQ(x->shares, y->shares);
        ASSERT_EQ(x->orders, y->orders);
      }
    }
    if (step % 50 == 0 && !refs.empty()) {
      const auto r = refs[static_cast<std::size_t>(rng.uniform(0, static_cast<std::int64_t>(refs.size()) - 1))];
      ASSERT_EQ(book.shares_ahead(r), ref.shares_ahead(r));
    }
  }
}

TEST(MdBookDifferential, MatchesReferenceAcrossSeedsAndCapacities) {
  DiffStats stats;
  for (std::uint64_t seed = 1; seed <= 10; ++seed) {
    run_differential(seed, 512, 64, 3000, stats);
    run_differential(seed + 100, 12, 4, 1500, stats);  // capacity edges
    if (HasFatalFailure()) return;
  }
  // The generator must exercise both outcomes substantially.
  EXPECT_GT(stats.applied, 10000);
  EXPECT_GT(stats.rejected, 5000);
}

}  // namespace
