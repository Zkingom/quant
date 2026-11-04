#pragma once

// Market-wide replay drivers (D-056).
//   shards == 0: single-threaded -- decode, route and apply in one thread.
//   shards == N: the calling thread decodes and routes; N worker threads,
//                each with its own SPSC queue and Shard (locate % N).
// Both modes use the same Router and Shard code; the queues are FIFO and
// lossless, so each instrument sees the same inputs in the same order.

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "lob/concurrent/spsc_queue.hpp"
#include "lob/md/market.hpp"
#include "lob/pipeline/pipeline.hpp"

namespace lob::pipeline {

struct MarketRun {
  RunStats stats;
  md::RouterCounters router;
  md::MarketSummary summary;
  std::vector<std::size_t> shard_peaks;       ///< peak live orders of each shard's pool
  std::vector<std::size_t> shard_instruments;
};

inline MarketRun run_market(ChunkSource& src, const md::MarketConfig& mc, std::size_t shards, const RunConfig& rc) {
  const std::size_t n = shards == 0 ? 1 : shards;
  std::vector<std::unique_ptr<md::Shard>> sh;
  for (std::size_t k = 0; k < n; ++k) sh.push_back(std::make_unique<md::Shard>(mc, k, n));
  md::Router router{n};
  MarketRun out;
  if (shards == 0) {
    out.stats = detail::produce(src, rc, [&](const Item& it) {
      router.on_input(it.in, [&](std::size_t, const md::Routed& r) { sh[0]->apply(r); });
    });
  } else {
    std::vector<std::unique_ptr<concurrent::SpscQueue<md::Routed>>> q;
    for (std::size_t k = 0; k < n; ++k) q.push_back(std::make_unique<concurrent::SpscQueue<md::Routed>>(rc.queue_capacity));
    std::atomic<bool> done{false};
    std::vector<std::thread> workers;
    for (std::size_t k = 0; k < n; ++k) {
      workers.emplace_back([&, k] {
        if (rc.hooks.begin != nullptr) rc.hooks.begin();
        md::Routed r;
        std::uint64_t spins = 0;
        for (;;) {
          if (q[k]->try_pop(r)) {
            sh[k]->apply(r);
          } else if (done.load(std::memory_order_acquire)) {
            // acquire pairs with the release below: every push is visible.
            while (q[k]->try_pop(r)) sh[k]->apply(r);
            break;
          } else {
            detail::relax(spins, rc.yield_every);
          }
        }
        if (rc.hooks.end != nullptr) rc.hooks.end();
      });
    }
    std::uint64_t full = 0;
    try {
      out.stats = detail::produce(src, rc, [&](const Item& it) {
        router.on_input(it.in, [&](std::size_t k, const md::Routed& r) {
          while (!q[k]->try_push(r)) detail::relax(full, rc.yield_every);
        });
      });
    } catch (...) {
      done.store(true, std::memory_order_release);
      for (auto& w : workers) w.join();
      throw;
    }
    done.store(true, std::memory_order_release);
    for (auto& w : workers) w.join();
    out.stats.producer_full_spins = full;
  }
  out.router = router.counters();
  out.summary = md::summarize(sh);
  for (const auto& s : sh) {
    out.shard_peaks.push_back(s->pool().peak());
    out.shard_instruments.push_back(s->instruments());
  }
  return out;
}

}  // namespace lob::pipeline
