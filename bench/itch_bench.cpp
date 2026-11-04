// lob_itch_bench: latency waterfall of the market-data pipeline on a
// deterministic synthetic ITCH stream (L3' stage 2; extended in stage 3).
// Methodology follows BENCHMARK §3: generation outside the timed region,
// a warm-up run, several repetitions, hashes checked across modes and
// repetitions, per-hop exact percentiles, clock overhead measured and
// subtracted. Wall-clock numbers are LOCAL ONLY (not in CI); they describe
// the machine they were taken on.
//
//   lob_itch_bench [--messages N] [--seed S] [--reps R] [--pace-ns NS] [--quick] [--write PATH]
//   --write PATH: only write the generated stream to PATH (a test input for lob_itch) and exit
//
// Modes: single (one thread), pipeline (saturated: the decoder runs ahead
// and the queue hop is dominated by queueing), paced (decoder held to one
// message per NS; the queue hop approaches the cross-core hand-off cost).

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "lob/pipeline/pipeline.hpp"
#include "lob/testkit/itch_generator.hpp"

namespace {

namespace pl = lob::pipeline;
namespace md = lob::md;
namespace tk = lob::testkit::itch;

struct Options {
  std::size_t messages{1'000'000};
  std::uint64_t seed{1};
  int reps{5};
  std::uint64_t pace_ns{1000};
  std::string write;
};

struct ModeResult {
  std::string name;
  std::vector<double> mps;  // frames per second per rep
  std::vector<pl::LatencyRecorder::Summary> hop[pl::kHopCount];
  std::uint64_t event_hash{0};
  std::uint64_t book_hash{0};
};

double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v.empty() ? 0.0 : v[v.size() / 2];
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string k = argv[i];
    auto val = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string("0"); };
    if (k == "--messages") o.messages = std::stoull(val());
    else if (k == "--seed") o.seed = std::stoull(val());
    else if (k == "--reps") o.reps = std::stoi(val());
    else if (k == "--pace-ns") o.pace_ns = std::stoull(val());
    else if (k == "--write") o.write = val();
    else if (k == "--quick") { o.messages = 20000; o.reps = 1; }
    else { std::fprintf(stderr, "unknown option %s\n", k.c_str()); return 2; }
  }

  tk::GeneratorParams gp;
  gp.seed = o.seed;
  gp.messages = o.messages;
  const tk::Buf stream = tk::generate_stream(gp);
  if (!o.write.empty()) {
    std::FILE* f = std::fopen(o.write.c_str(), "wb");
    if (f == nullptr || std::fwrite(stream.data(), 1, stream.size(), f) != stream.size() || std::fclose(f) != 0) {
      std::fprintf(stderr, "cannot write %s\n", o.write.c_str());
      return 1;
    }
    std::printf("wrote %zu bytes (%zu messages) to %s\n", stream.size(), o.messages, o.write.c_str());
    return 0;
  }
  md::FeedConfig cfg;
  cfg.symbol = md::make_symbol(tk::symbol_of(1));
  cfg.book = md::BookConfig{1U << 16, 1U << 12};

  const pl::ClockCalibration clock = pl::calibrate();
  {
    std::vector<std::uint64_t> d(20001);
    for (auto& x : d) {
      const std::uint64_t a = pl::steady_ns();
      x = pl::steady_ns() - a;
    }
    std::nth_element(d.begin(), d.begin() + 10000, d.end());
    std::printf("clock: ticks overhead %.1f ns (%.0f ticks), ns_per_tick %.4f; steady_clock pair %llu ns\n",
                clock.overhead_ticks * clock.ns_per_tick, clock.overhead_ticks, clock.ns_per_tick,
                static_cast<unsigned long long>(d[10000]));
  }
  std::printf("stream: %zu messages, %zu bytes, seed %llu, %d instruments (target SYM1)\n", o.messages,
              stream.size(), static_cast<unsigned long long>(o.seed), gp.instruments);

  const auto pace_ticks = static_cast<std::uint64_t>(static_cast<double>(o.pace_ns) / clock.ns_per_tick);
  struct Mode { const char* name; bool pipelined; std::uint64_t pace; };
  const Mode modes[] = {{"single", false, 0}, {"pipeline", true, 0}, {"paced", true, pace_ticks}};
  std::vector<ModeResult> results;
  for (const Mode& m : modes) {
    ModeResult r;
    r.name = m.name;
    for (int rep = -1; rep < o.reps; ++rep) {  // rep -1 = warm-up, not recorded
      pl::LatencyRecorder rec{o.messages + 2};
      pl::EventDigest digest;
      pl::BookStage<pl::EventDigest> stage{cfg, digest, &rec};
      pl::MemorySource src{stream};
      pl::RunConfig rc;
      rc.pace_ticks_per_msg = m.pace;
      rc.yield_every = 0;  // pure spin: 4+ vCPUs, two pipeline threads
      const std::uint64_t t0 = pl::steady_ns();
      const pl::RunStats st = m.pipelined ? pl::run_pipelined(src, stage, rc) : pl::run_single(src, stage, rc);
      const double secs = static_cast<double>(pl::steady_ns() - t0) / 1e9;
      const std::uint64_t bh = stage.feed().book().state_hash();
      if (rep >= 0 && (r.event_hash != 0 && (r.event_hash != digest.hash || r.book_hash != bh))) {
        std::fprintf(stderr, "FATAL: %s rep %d hash differs\n", m.name, rep);
        return 1;
      }
      r.event_hash = digest.hash;
      r.book_hash = bh;
      if (rep < 0) continue;
      r.mps.push_back(static_cast<double>(st.frames) / secs);
      for (int h = 0; h < pl::kHopCount; ++h) {
        r.hop[h].push_back(rec.summarize(static_cast<pl::Hop>(h), clock));
      }
    }
    results.push_back(std::move(r));
  }
  for (const auto& r : results) {
    if (r.event_hash != results[0].event_hash || r.book_hash != results[0].book_hash) {
      std::fprintf(stderr, "FATAL: mode %s differs from single\n", r.name.c_str());
      return 1;
    }
  }
  std::printf("event_hash=0x%016llx book_hash=0x%016llx (identical in all modes and reps)\n\n",
              static_cast<unsigned long long>(results[0].event_hash),
              static_cast<unsigned long long>(results[0].book_hash));
  std::printf("| mode | hop | p50 ns | p99 ns | p99.9 ns | max ns | p99 range over reps |\n");
  std::printf("|---|---|---|---|---|---|---|\n");
  for (const auto& r : results) {
    for (int h = 0; h < pl::kHopCount; ++h) {
      const auto& v = r.hop[h];
      if (v.empty() || v[0].n == 0) continue;
      std::vector<double> p50, p99, p999, mx;
      for (const auto& s : v) { p50.push_back(s.p50); p99.push_back(s.p99); p999.push_back(s.p999); mx.push_back(s.max); }
      std::printf("| %s | %s | %.0f | %.0f | %.0f | %.0f | %.0f–%.0f |\n", r.name.c_str(),
                  pl::hop_name(static_cast<pl::Hop>(h)), median(p50), median(p99), median(p999), median(mx),
                  *std::min_element(p99.begin(), p99.end()), *std::max_element(p99.begin(), p99.end()));
    }
  }
  std::printf("\n| mode | throughput (msg/s, median of %d) | range |\n|---|---|---|\n", o.reps);
  for (const auto& r : results) {
    std::printf("| %s | %.0f | %.0f–%.0f |\n", r.name.c_str(), median(r.mps),
                *std::min_element(r.mps.begin(), r.mps.end()), *std::max_element(r.mps.begin(), r.mps.end()));
  }

  return 0;
}
