// Fixed-seed differential test: OrderBook vs. ReferenceBook.
//
// A seeded generator issues a random mix of add / cancel / fill_front
// operations, including deliberate contract violations. For every operation:
//   - the reference model decides whether the call is legal;
//   - a legal call must succeed on OrderBook and return exactly what the
//     model returns; an illegal call must throw std::logic_error;
//   - afterwards the complete observable state of both books must be equal
//     and OrderBook::find_invariant_violation() must report nothing.
// The test also asserts that the generator actually reached each interesting
// path (capacity limit, id reuse, level removal, ...), so that a generator
// regression cannot silently weaken the test.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "book_test_util.hpp"
#include "lob/book/order_book.hpp"
#include "reference_book.hpp"
#include "lob/testkit/rng.hpp"

namespace lob {
namespace {

using test::ReferenceBook;

// Narrow band and small capacity so that levels collide, empty and refill,
// and the capacity limit is hit regularly.
constexpr EngineConfig kConfig{
    .min_price = 1, .max_price = 40, .max_order_quantity = 50, .max_live_orders = 40};

constexpr int kSeeds = 20;
constexpr int kStepsPerSeed = 5'000;

using testkit::Rng;

struct Coverage {
  long adds = 0;
  long adds_new_level = 0;
  long adds_reusing_dead_id = 0;
  long rejected_crossing = 0;
  long rejected_duplicate = 0;
  long rejected_capacity = 0;
  long rejected_invalid = 0;
  long rejected_stale_seq = 0;
  long cancels_hit = 0;
  long cancels_miss = 0;
  long fills_partial = 0;
  long fills_complete = 0;
  long fills_rejected = 0;
  long levels_removed = 0;
  std::size_t max_orders = 0;
};

// ------------------------------------------------------------ comparison

std::string describe(const OrderView& o) {
  std::ostringstream s;
  s << '{' << o.id << ' ' << to_string(o.side) << ' ' << o.price << " rem=" << o.remaining
    << " seq=" << o.seq << '}';
  return s.str();
}

::testing::AssertionResult same_state(const OrderBook& book, const ReferenceBook& ref) {
  if (const auto violation = book.find_invariant_violation()) {
    return ::testing::AssertionFailure() << "invariant: " << *violation;
  }
  if (book.order_count() != ref.order_count()) {
    return ::testing::AssertionFailure()
           << "order_count " << book.order_count() << " vs " << ref.order_count();
  }
  for (const Side side : {Side::Buy, Side::Sell}) {
    std::vector<OrderView> actual;
    book.for_each_order(side, [&](Price price, const RestingOrder& o) {
      actual.push_back(OrderView{
          .id = o.id, .side = side, .price = price, .remaining = o.remaining, .seq = o.seq});
    });
    const std::vector<OrderView> expected = ref.orders_in_priority(side);
    if (actual != expected) {
      auto failure = ::testing::AssertionFailure() << to_string(side) << " priority order differs:";
      for (std::size_t i = 0; i < std::max(actual.size(), expected.size()); ++i) {
        failure << "\n  [" << i << "] book=" << (i < actual.size() ? describe(actual[i]) : "-")
                << " ref=" << (i < expected.size() ? describe(expected[i]) : "-");
      }
      return failure;
    }
    const auto expected_depth = ref.depth(side);
    if (book.depth(side, std::numeric_limits<std::size_t>::max()) != expected_depth) {
      return ::testing::AssertionFailure() << to_string(side) << " depth differs";
    }
    if (book.level_count(side) != expected_depth.size()) {
      return ::testing::AssertionFailure() << to_string(side) << " level_count differs";
    }
    if (book.best_price(side) != ref.best_price(side) || book.front(side) != ref.front(side)) {
      return ::testing::AssertionFailure() << to_string(side) << " best/front differs";
    }
    for (const LevelSummary& level : expected_depth) {
      if (book.quantity_at(side, level.price) != level.total_quantity) {
        return ::testing::AssertionFailure() << "quantity_at " << level.price << " differs";
      }
    }
  }
  for (const OrderView& o : ref.orders()) {
    if (book.find(o.id) != o) {
      return ::testing::AssertionFailure() << "find(" << o.id << ") differs";
    }
  }
  return ::testing::AssertionSuccess();
}

// ------------------------------------------------------------ generator

/// A price that is legal for `side` given the current opposite best, or (if
/// the spread leaves no room) whatever the band allows; the model decides.
Price passive_price(Rng& rng, const ReferenceBook& ref, Side side) {
  if (side == Side::Buy) {
    const auto ask = ref.best_price(Side::Sell);
    const Price hi = ask ? std::max<Price>(kConfig.min_price, *ask - 1) : kConfig.max_price;
    return rng.uniform(std::max<Price>(kConfig.min_price, hi - 12), hi);
  }
  const auto bid = ref.best_price(Side::Buy);
  const Price lo = bid ? std::min<Price>(kConfig.max_price, *bid + 1) : kConfig.min_price;
  return rng.uniform(lo, std::min<Price>(kConfig.max_price, lo + 12));
}

class Driver {
 public:
  explicit Driver(std::uint64_t seed) : rng_(seed) {}

  ::testing::AssertionResult run(int steps, Coverage& cov) {
    for (int step = 0; step < steps; ++step) {
      const int roll = static_cast<int>(rng_.uniform(0, 99));
      std::string op;
      if (roll < 38) {
        op = step_add(cov);
      } else if (roll < 45) {
        op = step_bad_add(cov);
      } else if (roll < 70) {
        op = step_cancel(cov);
      } else {
        op = step_fill(cov);
      }
      if (!op_ok_) {
        return ::testing::AssertionFailure() << "step " << step << " " << op << ": " << op_error_;
      }
      if (auto same = same_state(book_, ref_); !same) {
        return ::testing::AssertionFailure() << "step " << step << " after " << op << ": "
                                             << same.message();
      }
      cov.max_orders = std::max(cov.max_orders, ref_.order_count());
    }
    return ::testing::AssertionSuccess();
  }

 private:
  // Applies an add to both books, or checks that both refuse it.
  // `reused_id` marks an id issued before (live -> duplicate, dead -> reuse).
  std::string apply_add(const NewOrder& order, SeqNum seq, bool reused_id, Coverage& cov) {
    std::ostringstream op;
    op << "add{" << order.id << ' ' << to_string(order.side) << ' ' << order.price << 'x'
       << order.quantity << " seq=" << seq << '}';
    const bool legal = ref_.can_add(order, seq);
    const bool new_level = ref_.quantity_at(order.side, order.price) == 0;
    try {
      book_.add(order, seq);
      if (!legal) {
        fail("book accepted an add the model forbids");
        return op.str();
      }
    } catch (const std::logic_error& e) {
      if (legal) {
        fail(std::string("book refused a legal add: ") + e.what());
      }
      return op.str();
    }
    ref_.add(order, seq);
    seq_ = seq;
    ++cov.adds;
    cov.adds_new_level += new_level ? 1 : 0;
    cov.adds_reusing_dead_id += reused_id ? 1 : 0;
    return op.str();
  }

  std::string step_add(Coverage& cov) {
    const Side side = rng_.side();
    // 15%: reuse an older id (dead -> legal reuse, live -> duplicate).
    const bool reused_id = next_id_ > 1 && rng_.percent(15);
    const OrderId id =
        reused_id ? static_cast<OrderId>(rng_.uniform(1, static_cast<std::int64_t>(next_id_) - 1))
                  : next_id_++;
    const NewOrder order{.id = id,
                         .side = side,
                         .price = passive_price(rng_, ref_, side),
                         .quantity = rng_.uniform(1, kConfig.max_order_quantity)};
    if (ref_.find(id)) {
      ++cov.rejected_duplicate;
    } else if (static_cast<std::int64_t>(ref_.order_count()) >= kConfig.max_live_orders) {
      ++cov.rejected_capacity;
    }
    return apply_add(order, seq_ + 1, reused_id, cov);
  }

  std::string step_bad_add(Coverage& cov) {
    const Side side = rng_.side();
    NewOrder order{.id = next_id_++,
                   .side = side,
                   .price = passive_price(rng_, ref_, side),
                   .quantity = rng_.uniform(1, kConfig.max_order_quantity)};
    SeqNum seq = seq_ + 1;
    switch (rng_.uniform(0, 3)) {
      case 0: {  // cross or lock the opposite best
        const auto best = ref_.best_price(opposite(side));
        if (!best) {
          return "bad_add(skip: opposite side empty)";
        }
        order.price = side == Side::Buy ? rng_.uniform(*best, kConfig.max_price)
                                        : rng_.uniform(kConfig.min_price, *best);
        ++cov.rejected_crossing;
        break;
      }
      case 1:  // duplicate of a live id
        if (ref_.orders().empty()) {
          return "bad_add(skip: book empty)";
        }
        order.id = ref_.orders()[static_cast<std::size_t>(
                                     rng_.uniform(0, static_cast<std::int64_t>(ref_.order_count()) - 1))]
                       .id;
        ++cov.rejected_duplicate;
        break;
      case 2:  // stale seq
        seq = static_cast<SeqNum>(rng_.uniform(0, static_cast<std::int64_t>(seq_)));
        ++cov.rejected_stale_seq;
        break;
      default:  // stateless-invalid field
        order.quantity = rng_.percent(50) ? 0 : kConfig.max_order_quantity + 1;
        ++cov.rejected_invalid;
        break;
    }
    if (ref_.can_add(order, seq)) {
      fail("generator produced a legal 'bad' add");
    }
    return "bad_" + apply_add(order, seq, false, cov);
  }

  std::string step_cancel(Coverage& cov) {
    // 60%: a live id (any queue position); otherwise uniform over every id
    // issued so far plus a few never issued (dead / unknown cancels).
    OrderId id = 0;
    if (!ref_.orders().empty() && rng_.percent(60)) {
      const auto pick = rng_.uniform(0, static_cast<std::int64_t>(ref_.order_count()) - 1);
      id = ref_.orders()[static_cast<std::size_t>(pick)].id;
    } else {
      id = static_cast<OrderId>(rng_.uniform(1, static_cast<std::int64_t>(next_id_) + 2));
    }
    const std::size_t levels_before = total_levels();
    const auto expected = ref_.cancel(id);
    const auto actual = book_.cancel(id);
    if (actual != expected) {
      fail("cancel result differs");
    }
    ++(expected ? cov.cancels_hit : cov.cancels_miss);
    cov.levels_removed += static_cast<long>(levels_before - total_levels());
    return "cancel{" + std::to_string(id) + "}";
  }

  std::string step_fill(Coverage& cov) {
    const Side side = rng_.side();
    const auto front = ref_.front(side);
    Quantity qty = 1;
    if (front) {
      const int roll = static_cast<int>(rng_.uniform(0, 99));
      if (roll < 25) {
        qty = front->remaining;  // complete
      } else if (roll < 32) {
        qty = rng_.percent(50) ? 0 : front->remaining + 1;  // illegal
      } else {
        qty = rng_.uniform(1, front->remaining);
      }
    }
    const std::string op = "fill{" + std::string(to_string(side)) + ' ' + std::to_string(qty) + '}';
    const bool legal = ref_.can_fill(side, qty);
    const std::size_t levels_before = total_levels();
    try {
      const Fill actual = book_.fill_front(side, qty);
      if (!legal) {
        fail("book accepted an illegal fill");
        return op;
      }
      const Fill expected = ref_.fill(side, qty);
      if (actual != expected) {
        fail("fill result differs");
      }
      ++(expected.maker_remaining == 0 ? cov.fills_complete : cov.fills_partial);
    } catch (const std::logic_error& e) {
      if (legal) {
        fail(std::string("book refused a legal fill: ") + e.what());
      }
      ++cov.fills_rejected;
    }
    cov.levels_removed += static_cast<long>(levels_before - total_levels());
    return op;
  }

  std::size_t total_levels() const {
    return book_.level_count(Side::Buy) + book_.level_count(Side::Sell);
  }

  void fail(std::string message) {
    op_ok_ = false;
    op_error_ = std::move(message);
  }

  Rng rng_;
  OrderBook book_{kConfig};
  ReferenceBook ref_{kConfig};
  SeqNum seq_{0};
  OrderId next_id_{1};
  bool op_ok_{true};
  std::string op_error_;
};

TEST(OrderBookDifferential, MatchesReferenceModelOnFixedSeeds) {
  Coverage cov;
  for (int seed = 1; seed <= kSeeds; ++seed) {
    Driver driver{static_cast<std::uint64_t>(seed)};
    ASSERT_TRUE(driver.run(kStepsPerSeed, cov)) << "seed " << seed;
  }

  std::printf(
      "[coverage] seeds=%d steps/seed=%d adds=%ld new_level=%ld reused_dead_id=%ld\n"
      "[coverage] rejected: crossing=%ld duplicate=%ld capacity=%ld invalid=%ld stale_seq=%ld\n"
      "[coverage] cancels hit=%ld miss=%ld; fills partial=%ld complete=%ld rejected=%ld\n"
      "[coverage] levels_removed=%ld max_orders=%zu\n",
      kSeeds, kStepsPerSeed, cov.adds, cov.adds_new_level, cov.adds_reusing_dead_id,
      cov.rejected_crossing, cov.rejected_duplicate, cov.rejected_capacity, cov.rejected_invalid,
      cov.rejected_stale_seq, cov.cancels_hit, cov.cancels_miss, cov.fills_partial,
      cov.fills_complete, cov.fills_rejected, cov.levels_removed, cov.max_orders);

  // Guard against a generator that silently stops exercising a path.
  EXPECT_GT(cov.adds_new_level, 0);
  EXPECT_GT(cov.adds - cov.adds_new_level, 0);  // joined an existing level
  EXPECT_GT(cov.adds_reusing_dead_id, 0);
  EXPECT_GT(cov.rejected_crossing, 0);
  EXPECT_GT(cov.rejected_duplicate, 0);
  EXPECT_GT(cov.rejected_capacity, 0);
  EXPECT_GT(cov.rejected_invalid, 0);
  EXPECT_GT(cov.rejected_stale_seq, 0);
  EXPECT_GT(cov.cancels_hit, 0);
  EXPECT_GT(cov.cancels_miss, 0);
  EXPECT_GT(cov.fills_partial, 0);
  EXPECT_GT(cov.fills_complete, 0);
  EXPECT_GT(cov.fills_rejected, 0);
  EXPECT_GT(cov.levels_removed, 0);
  EXPECT_EQ(cov.max_orders, static_cast<std::size_t>(kConfig.max_live_orders));
}

// The reference model is itself code; pin its priority rule on a case worked
// out by hand so that a bug cannot be shared by model and book unnoticed.
TEST(ReferenceBook, PriorityRuleWorkedExample) {
  ReferenceBook ref{kConfig};
  ref.add(test::buy(1, 10, 5), 1);
  ref.add(test::buy(2, 12, 5), 2);
  ref.add(test::buy(3, 12, 5), 3);
  ref.add(test::sell(4, 20, 5), 4);
  ref.add(test::sell(5, 15, 5), 5);
  std::vector<OrderId> bids;
  for (const auto& o : ref.orders_in_priority(Side::Buy)) {
    bids.push_back(o.id);
  }
  EXPECT_EQ(bids, (std::vector<OrderId>{2, 3, 1}));
  EXPECT_EQ(ref.front(Side::Sell)->id, 5U);
  EXPECT_FALSE(ref.can_add(test::buy(6, 15, 1), 6));  // locks best ask
  EXPECT_TRUE(ref.can_add(test::buy(6, 14, 1), 6));
}

}  // namespace
}  // namespace lob
