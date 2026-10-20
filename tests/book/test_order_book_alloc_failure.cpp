// Allocation-failure injection for OrderBook's exception-safety guarantees.
//
// Uses the shared harness in support/alloc_fault.{hpp,cpp}, which replaces
// the global operator new/delete for this executable only
// (lob_book_alloc_tests). Injection is disarmed by default; a FaultWindow
// arms it around exactly one book operation and fails at most one
// allocation, so GoogleTest and the test's own bookkeeping never see an
// injected failure.
//
// For an operation we sweep k = 0, 1, 2, ...: rebuild the same book, arm
// "fail the k-th allocation", run the operation. While the operation throws
// std::bad_alloc we check the strong guarantee (book byte-identical to before,
// invariants hold, the same operation then succeeds without injection). The
// first k for which the operation completes equals the number of allocations
// the operation performs, which proves every allocation point was injected.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <new>
#include <stdexcept>
#include <string>

#include "book_test_util.hpp"
#include "lob/book/order_book.hpp"
#include "support/alloc_fault.hpp"

using lob::test::alloc::FaultWindow;

namespace lob {
namespace {

using test::buy;
using test::dump;
using test::sell;

constexpr EngineConfig kConfig{
    .min_price = 1, .max_price = 1'000, .max_order_quantity = 100, .max_live_orders = 1'000};

using Setup = std::function<OrderBook()>;
using Operation = std::function<void(OrderBook&)>;

struct SweepResult {
  long injection_points = 0;     // k values for which bad_alloc was injected
  long clean_allocations = -1;   // allocations of the successful run
};

SweepResult sweep(const Setup& setup, const Operation& op, const std::string& label) {
  SweepResult result;
  for (long k = 0; k < 1'000; ++k) {
    OrderBook book = setup();
    const std::string before = dump(book);
    bool threw = false;
    long allocations = 0;
    {
      FaultWindow window{k};
      try {
        op(book);
      } catch (const std::bad_alloc&) {
        threw = true;
      }
      allocations = window.allocations();
      EXPECT_EQ(window.injected(), threw) << label << " k=" << k;
    }
    if (!threw) {
      result.clean_allocations = allocations;
      break;
    }
    ++result.injection_points;
    EXPECT_EQ(dump(book), before) << label << ": book changed after bad_alloc at k=" << k;
    EXPECT_BOOK_CONSISTENT(book) << label << " k=" << k;
    // Still fully usable: the same operation succeeds without injection.
    EXPECT_NO_THROW(op(book)) << label << " k=" << k;
    EXPECT_BOOK_CONSISTENT(book) << label << " k=" << k;
  }
  EXPECT_GE(result.clean_allocations, 0) << label << ": sweep did not terminate";
  EXPECT_EQ(result.injection_points, result.clean_allocations)
      << label << ": not every allocation point was injected";
  return result;
}

OrderBook book_with(int resting_orders) {
  OrderBook book{kConfig};
  for (int i = 0; i < resting_orders; ++i) {
    // Buys on 4 price levels so new orders both join and create levels.
    book.add(buy(static_cast<OrderId>(i + 1), 100 - (i % 4), 1), static_cast<SeqNum>(i + 1));
  }
  book.add(sell(999, 200, 5), static_cast<SeqNum>(resting_orders + 1));
  return book;
}

// Volatile sink: the compiler may elide a new-expression whose result is
// unused ([expr.new]/10), which would bypass the injector.
int* volatile g_sink = nullptr;

TEST(OrderBookAllocFailure, InjectionHarnessWorks) {
  FaultWindow window{0};
  EXPECT_THROW(g_sink = new int(1), std::bad_alloc);
  EXPECT_TRUE(window.injected());
  g_sink = new int(2);  // window disarmed itself after one failure
  delete g_sink;
}

TEST(OrderBookAllocFailure, AddNewLevelOnEmptyBookHasStrongGuarantee) {
  const auto r = sweep([] { return OrderBook{kConfig}; },
                       [](OrderBook& b) { b.add(buy(1, 100, 5), 1); }, "add to empty book");
  std::printf("[alloc] add to empty book: %ld allocations\n", r.injection_points);
  // map node + list node + index node + first index bucket array.
  EXPECT_EQ(r.injection_points, 4);
}

TEST(OrderBookAllocFailure, AddToExistingLevelHasStrongGuarantee) {
  const auto r = sweep([] { return book_with(4); },
                       [](OrderBook& b) { b.add(buy(50, 100, 5), 100); }, "add to level 100");
  std::printf("[alloc] add to existing level: %ld allocations\n", r.injection_points);
  EXPECT_EQ(r.injection_points, 2);  // list node + index node (no rehash at this size)
}

TEST(OrderBookAllocFailure, AddSweepAcrossBookSizesIncludingRehash) {
  // Growing the book crosses several unordered_map rehash thresholds, so
  // some of these adds allocate a new bucket array as well.
  long total_points = 0;
  long max_points = 0;
  for (int n = 0; n <= 64; ++n) {
    const SeqNum seq = static_cast<SeqNum>(n + 2);
    const Price price = 100 - (n % 6);  // levels 95/94 are new for small n
    const auto r = sweep([n] { return book_with(n); },
                         [seq, price](OrderBook& b) { b.add(buy(500, price, 1), seq); },
                         "add at size " + std::to_string(n));
    total_points += r.injection_points;
    max_points = std::max(max_points, r.injection_points);
  }
  std::printf("[alloc] add sweep over sizes 0..64: %ld injection points, max %ld per add\n",
              total_points, max_points);
  EXPECT_GT(max_points, 3);  // at least one add needed a rehash allocation
}

TEST(OrderBookAllocFailure, FillAndCancelNeverAllocate) {
  struct Case {
    const char* label;
    Operation op;
  };
  const Case cases[] = {
      {"partial fill", [](OrderBook& b) { (void)b.fill_front(Side::Sell, 1); }},
      {"complete fill keeping level", [](OrderBook& b) { (void)b.fill_front(Side::Buy, 1); }},
      {"complete fill removing level", [](OrderBook& b) { (void)b.fill_front(Side::Sell, 5); }},
      {"cancel head", [](OrderBook& b) { (void)b.cancel(1); }},
      {"cancel removing level", [](OrderBook& b) { (void)b.cancel(999); }},
      {"cancel unknown", [](OrderBook& b) { (void)b.cancel(12345); }},
  };
  for (const Case& c : cases) {
    OrderBook book = book_with(8);
    FaultWindow window{0};  // the very first allocation would fail
    EXPECT_NO_THROW(c.op(book)) << c.label;
    EXPECT_EQ(window.allocations(), 0) << c.label;
    EXPECT_FALSE(window.injected()) << c.label;
  }
}

TEST(OrderBookAllocFailure, ContractViolationUnderMemoryPressureLeavesBookUnchanged) {
  // Building the logic_error message allocates; if that allocation fails the
  // caller sees bad_alloc instead, but the check still precedes any mutation.
  for (long k = 0; k < 4; ++k) {
    OrderBook book = book_with(4);
    const std::string before = dump(book);
    {
      FaultWindow window{k};
      EXPECT_ANY_THROW(book.add(buy(1, 90, 5), 100));  // duplicate id
    }
    EXPECT_EQ(dump(book), before) << "k=" << k;
    EXPECT_BOOK_CONSISTENT(book);
  }
}

}  // namespace
}  // namespace lob
