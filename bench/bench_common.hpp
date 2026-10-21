#pragma once

// Shared measurement code for lob_bench (BENCHMARK.md §3).
//
// Output is CSV on stdout, one value per line:
//   <scenario>,<metric>,<rep>,<value>
// metric: throughput (operations per second, from an untimed loop),
// lat_p50/lat_p95/lat_p99/lat_p999/lat_max/lat_mean (ns, from a separate
// per-operation-timed repetition; include the clock overhead), samples,
// clock_pair_ns, checksum. Aggregation happens in scripts/bench_report.py.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace lob::bench {

using Clock = std::chrono::steady_clock;

inline std::int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

struct Options {
  int reps = 5;                 ///< throughput repetitions per scenario
  std::string filter;           ///< run only scenarios containing this substring
  std::size_t engine_commands = 200'000;
  std::size_t durable_commands = 5'000;
  std::string ninep_dir;        ///< directory on 9P for the durable 9P session run ("" = skip)
  bool quick = false;           ///< tiny sizes, for the smoke test
};

/// Global checksum: every measured result is folded in and printed at the
/// end, so the compiler cannot discard the measured work.
inline std::uint64_t g_checksum = 0;
inline void consume(std::uint64_t v) { g_checksum = g_checksum * 0x100000001b3ULL ^ v; }

inline void emit(const std::string& scenario, const char* metric, int rep, double value) {
  std::printf("%s,%s,%d,%.6g\n", scenario.c_str(), metric, rep, value);
}

/// Exact percentiles of the latency samples (sorted copy).
inline void emit_latency(const std::string& scenario, std::vector<std::int64_t> ns) {
  if (ns.empty()) {
    return;
  }
  std::sort(ns.begin(), ns.end());
  auto pct = [&](double p) {
    const auto i = static_cast<std::size_t>(std::ceil(p * static_cast<double>(ns.size()))) - 1;
    return static_cast<double>(ns[std::min(i, ns.size() - 1)]);
  };
  double sum = 0;
  for (const auto v : ns) {
    sum += static_cast<double>(v);
  }
  emit(scenario, "samples", 0, static_cast<double>(ns.size()));
  emit(scenario, "lat_p50", 0, pct(0.50));
  emit(scenario, "lat_p95", 0, pct(0.95));
  emit(scenario, "lat_p99", 0, pct(0.99));
  if (ns.size() >= 10'000) {
    emit(scenario, "lat_p999", 0, pct(0.999));
  }
  emit(scenario, "lat_max", 0, static_cast<double>(ns.back()));
  emit(scenario, "lat_mean", 0, sum / static_cast<double>(ns.size()));
}

/// Median of back-to-back clock reads (the per-sample timing overhead).
inline double clock_pair_ns() {
  std::vector<std::int64_t> d(100'000);
  for (auto& v : d) {
    const auto a = now_ns();
    v = now_ns() - a;
  }
  std::nth_element(d.begin(), d.begin() + static_cast<std::ptrdiff_t>(d.size() / 2), d.end());
  return static_cast<double>(d[d.size() / 2]);
}

inline bool selected(const Options& o, const std::string& scenario) {
  return o.filter.empty() || scenario.find(o.filter) != std::string::npos;
}

/// Throughput: `reps` untimed-inside repetitions of `body()` (which performs
/// `ops` operations and returns a checksum); `setup()` runs untimed before
/// each repetition. One warmup repetition first.
template <typename Setup, typename Body>
void measure_throughput(const std::string& scenario, const Options& o, std::size_t ops, Setup setup, Body body) {
  setup();
  consume(body());  // warmup
  for (int rep = 1; rep <= o.reps; ++rep) {
    setup();
    const auto t0 = now_ns();
    const std::uint64_t r = body();
    const auto t1 = now_ns();
    consume(r);
    emit(scenario, "throughput", rep, static_cast<double>(ops) * 1e9 / static_cast<double>(t1 - t0));
  }
}

}  // namespace lob::bench
