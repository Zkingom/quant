// Book layer (M2): add / cancel at a given depth, fill_front on a long FIFO
// queue, queries. All operation parameters (prices, ids, which order to
// cancel) are generated before timing; the timed loops only call OrderBook.

#include <optional>
#include <string>
#include <vector>

#include "bench_common.hpp"
#include "lob/book/order_book.hpp"
#include "lob/testkit/rng.hpp"

namespace lob::bench {

namespace {

constexpr Price kMid = 50'000;
constexpr EngineConfig book_config(std::size_t depth) {
  return EngineConfig{.min_price = 1,
                      .max_price = 2 * kMid,
                      .max_order_quantity = 100,
                      .max_live_orders = static_cast<std::int64_t>(2 * depth + 16)};
}

/// A non-crossing order: bids strictly below kMid, asks strictly above.
NewOrder passive_order(testkit::Rng& rng, OrderId id) {
  const Side side = rng.side();
  const Price price = side == Side::Buy ? rng.uniform(1, kMid - 1) : rng.uniform(kMid + 1, 2 * kMid);
  return NewOrder{.id = id, .side = side, .price = price, .quantity = rng.uniform(1, 100)};
}

struct AddCancelPlan {
  std::vector<NewOrder> initial;  // the book at depth N
  std::vector<NewOrder> adds;     // pair i: add adds[i] ...
  std::vector<OrderId> cancels;   // ... then cancel cancels[i] (resting at that moment)
};

AddCancelPlan plan_add_cancel(std::size_t depth, std::size_t pairs) {
  testkit::Rng rng{7};
  AddCancelPlan p;
  std::vector<OrderId> live;
  OrderId next = 1;
  for (std::size_t i = 0; i < depth; ++i) {
    p.initial.push_back(passive_order(rng, next));
    live.push_back(next++);
  }
  for (std::size_t i = 0; i < pairs; ++i) {
    p.adds.push_back(passive_order(rng, next));
    live.push_back(next++);
    const auto k = static_cast<std::size_t>(rng.uniform(0, static_cast<std::int64_t>(live.size()) - 1));
    p.cancels.push_back(live[k]);
    live[k] = live.back();
    live.pop_back();
  }
  return p;
}

std::uint64_t book_digest(const OrderBook& b) {
  std::uint64_t h = 1469598103934665603ULL;
  for (const Side s : {Side::Buy, Side::Sell}) {
    b.for_each_order(s, [&](Price price, const RestingOrder& o) {
      h = (h ^ (o.id + static_cast<std::uint64_t>(price) * 31 + static_cast<std::uint64_t>(o.remaining))) *
          1099511628211ULL;
    });
  }
  return h;
}

void add_cancel(const Options& o, std::size_t depth) {
  const std::string name = "book.add_cancel@" + std::to_string(depth);
  if (!selected(o, name)) {
    return;
  }
  const std::size_t pairs = o.quick ? 2'000 : 200'000;
  const AddCancelPlan plan = plan_add_cancel(depth, pairs);
  const EngineConfig config = book_config(depth);
  std::optional<OrderBook> book;
  SeqNum seq = 0;
  auto setup = [&] {
    book.emplace(config);
    seq = 0;
    for (const NewOrder& n : plan.initial) {
      book->add(n, ++seq);
    }
  };
  std::optional<std::uint64_t> digest;
  measure_throughput(name, o, 2 * pairs, setup, [&] {
    for (std::size_t i = 0; i < pairs; ++i) {
      book->add(plan.adds[i], ++seq);
      (void)book->cancel(plan.cancels[i]);
    }
    const std::uint64_t d = book_digest(*book);
    if (digest && *digest != d) {
      std::fprintf(stderr, "%s: non-deterministic final book\n", name.c_str());
      std::abort();
    }
    digest = d;
    return d;
  });

  // Latency: add and cancel timed separately.
  setup();
  std::vector<std::int64_t> add_ns;
  std::vector<std::int64_t> cancel_ns;
  add_ns.reserve(pairs);
  cancel_ns.reserve(pairs);
  for (std::size_t i = 0; i < pairs; ++i) {
    const auto t0 = now_ns();
    book->add(plan.adds[i], ++seq);
    const auto t1 = now_ns();
    (void)book->cancel(plan.cancels[i]);
    const auto t2 = now_ns();
    add_ns.push_back(t1 - t0);
    cancel_ns.push_back(t2 - t1);
  }
  consume(book_digest(*book));
  // Book shape (how often an add opens a new level depends on it).
  emit(name, "levels", 0, static_cast<double>(book->level_count(Side::Buy) + book->level_count(Side::Sell)));
  emit(name, "orders", 0, static_cast<double>(book->order_count()));
  emit_latency(name + ".add", std::move(add_ns));
  emit_latency(name + ".cancel", std::move(cancel_ns));
}

void fill_front(const Options& o) {
  const std::string name = "book.fill_front@queue100000";
  if (!selected(o, name)) {
    return;
  }
  const std::size_t q = o.quick ? 2'000 : 100'000;
  const EngineConfig config = book_config(q);
  testkit::Rng rng{7};
  std::vector<Quantity> qty(q);
  for (auto& v : qty) {
    v = rng.uniform(1, 100);
  }
  std::optional<OrderBook> book;
  auto setup = [&] {
    book.emplace(config);
    for (std::size_t i = 0; i < q; ++i) {
      book->add(NewOrder{.id = i + 1, .side = Side::Sell, .price = kMid, .quantity = qty[i]}, i + 1);
    }
  };
  measure_throughput(name, o, q, setup, [&] {
    std::uint64_t sum = 0;
    for (std::size_t i = 0; i < q; ++i) {
      sum += static_cast<std::uint64_t>(book->fill_front(Side::Sell, qty[i]).quantity);
    }
    return sum;
  });
  setup();
  std::vector<std::int64_t> ns;
  ns.reserve(q);
  for (std::size_t i = 0; i < q; ++i) {
    const auto t0 = now_ns();
    const Fill f = book->fill_front(Side::Sell, qty[i]);
    const auto t1 = now_ns();
    consume(static_cast<std::uint64_t>(f.quantity));
    ns.push_back(t1 - t0);
  }
  emit_latency(name, std::move(ns));
}

void queries(const Options& o, std::size_t depth) {
  const std::string name = "book.query@" + std::to_string(depth);
  if (!selected(o, name)) {
    return;
  }
  const std::size_t n = o.quick ? 2'000 : 200'000;
  const EngineConfig config = book_config(depth);
  const AddCancelPlan plan = plan_add_cancel(depth, 0);
  OrderBook book{config};
  SeqNum seq = 0;
  for (const NewOrder& x : plan.initial) {
    book.add(x, ++seq);
  }
  // Query mix: best_price, front, quantity_at(existing price | random price).
  testkit::Rng rng{11};
  struct Q {
    int kind;
    Side side;
    Price price;
  };
  std::vector<Q> qs(n);
  for (auto& q : qs) {
    q.kind = static_cast<int>(rng.uniform(0, 2));
    q.side = rng.side();
    q.price = rng.percent(50) ? plan.initial[static_cast<std::size_t>(rng.uniform(0, static_cast<std::int64_t>(depth) - 1))].price
                              : rng.uniform(1, 2 * kMid);
  }
  auto run_one = [&](const Q& q) -> std::uint64_t {
    switch (q.kind) {
      case 0:
        return static_cast<std::uint64_t>(book.best_price(q.side).value_or(0));
      case 1: {
        const auto f = book.front(q.side);
        return f ? f->id : 0;
      }
      default:
        return static_cast<std::uint64_t>(book.quantity_at(q.side, q.price));
    }
  };
  measure_throughput(name, o, n, [] {}, [&] {
    std::uint64_t sum = 0;
    for (const Q& q : qs) {
      sum += run_one(q);
    }
    return sum;
  });
  std::vector<std::int64_t> ns;
  ns.reserve(n);
  for (const Q& q : qs) {
    const auto t0 = now_ns();
    const std::uint64_t r = run_one(q);
    const auto t1 = now_ns();
    consume(r);
    ns.push_back(t1 - t0);
  }
  emit_latency(name, std::move(ns));
}

}  // namespace

void run_book(const Options& o) {
  add_cancel(o, 1'000);
  add_cancel(o, 100'000);
  fill_front(o);
  queries(o, 1'000);
  queries(o, 100'000);
}

}  // namespace lob::bench
