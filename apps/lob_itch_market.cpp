// lob_itch_market: market-wide ITCH replay -- every instrument's book (D-056).
//
//   lob_itch_market --input PATH|- [--shards N] [--max-orders N] [--queue N] [--hashes PATH]
//   lob_itch_market --input PATH|- --bench-bytes B [--max-orders N]

//
// Replay mode streams the input (bounded memory; nothing stored) with
// --shards 0 (default: single-threaded) or N worker shards, and prints the
// frame count, instruments, peak live orders, the distribution of final
// states and Stale reasons, and the order-independent summary hash.
// --hashes writes one line per instrument (locate symbol state events
// event_hash book_hash) so that two modes can be compared with diff.
//
// Bench mode reads the first B bytes of the input into memory (cut at the
// last complete frame), then replays them single-threaded and with
// N = 1, 2, 4 shards, checks that all summary hashes agree, and prints the
// throughput of each. Wall-clock numbers: local only, environment-specific.

#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "lob/pipeline/market_pipeline.hpp"

namespace {

namespace md = lob::md;
namespace pl = lob::pipeline;

struct Args {
  std::string input;
  std::size_t shards{0};
  std::size_t max_orders{1U << 23};
  std::size_t queue{1U << 14};
  std::string hashes;
  std::size_t bench_bytes{0};
};

[[noreturn]] void usage(const char* m) {
  std::fprintf(stderr, "lob_itch_market: %s (see apps/lob_itch_market.cpp)\n", m);
  std::exit(2);
}

long peak_rss_kib() {
  rusage ru{};
  getrusage(RUSAGE_SELF, &ru);
  return ru.ru_maxrss;
}

void print_summary(const pl::MarketRun& r, double secs, const std::string& label) {
  const auto& s = r.summary;
  std::printf("[%s] frames=%llu routed=%llu broadcasts=%llu faults=%llu skipped=%llu\n", label.c_str(),
              static_cast<unsigned long long>(r.router.frames), static_cast<unsigned long long>(r.router.routed),
              static_cast<unsigned long long>(r.router.broadcasts), static_cast<unsigned long long>(r.router.faults),
              static_cast<unsigned long long>(r.router.skipped));
  std::size_t active = 0;
  std::uint64_t applied = 0, crossed = 0, locked = 0, graces = 0, resolved = 0;
  for (const auto& st : s.stocks) {
    active += st.counters.applied > 0 ? 1U : 0U;
    applied += st.counters.applied;
    crossed += st.counters.crossed_updates;
    locked += st.counters.locked_updates;
    graces += st.counters.graces;
    resolved += st.counters.graces_resolved;
  }
  std::printf("[%s] instruments=%zu with_orders=%zu applied=%llu unknown_locate=%llu\n", label.c_str(), s.stocks.size(),
              active, static_cast<unsigned long long>(applied), static_cast<unsigned long long>(s.unknown_locate));
  std::printf("[%s] final states: live=%zu stale=%zu uninitialized=%zu other=%zu\n", label.c_str(), s.live, s.stale,
              s.uninitialized, s.other);
  for (const auto& [reason, n] : s.stale_reasons) std::printf("[%s]   stale reason %s: %zu\n", label.c_str(), reason.c_str(), n);
  std::printf("[%s] locked_updates=%llu crossed_updates=%llu resume_graces=%llu resolved=%llu\n", label.c_str(),
              static_cast<unsigned long long>(locked), static_cast<unsigned long long>(crossed),
              static_cast<unsigned long long>(graces), static_cast<unsigned long long>(resolved));
  for (const auto& st : s.stocks) {
    if (st.counters.graces > 0) {
      std::printf("[%s]   resume grace: %s (locate %u) graces=%llu resolved=%llu crossed_updates=%llu\n", label.c_str(),
                  st.symbol.c_str(), st.locate, static_cast<unsigned long long>(st.counters.graces),
                  static_cast<unsigned long long>(st.counters.graces_resolved),
                  static_cast<unsigned long long>(st.counters.crossed_updates));
    }
  }
  std::size_t listed = 0;
  for (const auto& st : s.stocks) {
    if (st.state == md::FeedState::Stale && listed++ < 50) {
      std::printf("[%s]   stale: %s (locate %u) reason %s\n", label.c_str(), st.symbol.c_str(), st.locate,
                  std::string(md::to_string(st.stale_reason)).c_str());
    }
  }
  {
    std::vector<std::size_t> lv;
    std::size_t sum = 0;
    std::string top;
    for (const auto& st : s.stocks) {
      lv.push_back(st.peak_levels);
      sum += st.peak_levels;
    }
    std::sort(lv.begin(), lv.end());
    if (!lv.empty()) {
      for (const auto& st : s.stocks) {
        if (st.peak_levels == lv.back()) top = st.symbol;
      }
      std::printf("[%s] peak levels per side per instrument: max=%zu (%s) p99.9=%zu p99=%zu p50=%zu sum=%zu\n",
                  label.c_str(), lv.back(), top.c_str(), lv[lv.size() * 999 / 1000], lv[lv.size() * 99 / 100],
                  lv[lv.size() / 2], sum);
    }
  }
  std::printf("[%s] peak_live_orders:", label.c_str());
  for (const auto p : r.shard_peaks) std::printf(" %zu", p);
  std::printf(" (sum %zu%s)\n", s.pool_peak_sum, r.shard_peaks.size() == 1 ? ", exact market peak" : ", upper bound");
  std::printf("[%s] summary_hash=0x%016llx elapsed_s=%.3f frames_per_s=%.0f peak_rss_kib=%ld\n", label.c_str(),
              static_cast<unsigned long long>(s.summary_hash), secs,
              static_cast<double>(r.router.frames) / secs, peak_rss_kib());
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string k = argv[i];
    auto val = [&]() -> std::string {
      if (i + 1 >= argc) usage(("missing value for " + k).c_str());
      return argv[++i];
    };
    if (k == "--input") a.input = val();
    else if (k == "--shards") a.shards = std::stoull(val());
    else if (k == "--max-orders") a.max_orders = std::stoull(val());
    else if (k == "--queue") a.queue = std::stoull(val());
    else if (k == "--hashes") a.hashes = val();
    else if (k == "--bench-bytes") a.bench_bytes = std::stoull(val());

    else usage(("unknown option " + k).c_str());
  }
  if (a.input.empty()) usage("--input is required");
  const int fd = a.input == "-" ? 0 : ::open(a.input.c_str(), O_RDONLY);
  if (fd < 0) {
    std::perror(a.input.c_str());
    return 1;
  }
  md::MarketConfig mc;
  pl::RunConfig rc;
  rc.queue_capacity = a.queue;

  try {
    if (a.bench_bytes == 0) {
      mc.max_orders_per_shard = a.max_orders;
      pl::FdSource src{fd};
      const std::uint64_t t0 = pl::steady_ns();
      const pl::MarketRun r = pl::run_market(src, mc, a.shards, rc);
      const double secs = static_cast<double>(pl::steady_ns() - t0) / 1e9;
      print_summary(r, secs, a.shards == 0 ? "single" : "shards=" + std::to_string(a.shards));
      if (!a.hashes.empty()) {
        std::ofstream out(a.hashes);
        for (const auto& s : r.summary.stocks) {
          char line[160];
          std::snprintf(line, sizeof line, "%u %s %s %llu %016llx %016llx\n", s.locate, s.symbol.c_str(),
                        std::string(md::to_string(s.state)).c_str(), static_cast<unsigned long long>(s.events),
                        static_cast<unsigned long long>(s.event_hash), static_cast<unsigned long long>(s.book_hash));
          out << line;
        }
      }
      return 0;
    }
    // Bench mode: a prefix of the stream in memory, cut at a frame boundary.
    std::vector<std::uint8_t> buf(a.bench_bytes);
    std::size_t got = 0;
    while (got < buf.size()) {
      const ssize_t r = ::read(fd, buf.data() + got, buf.size() - got);
      if (r <= 0) break;
      got += static_cast<std::size_t>(r);
    }
    md::itch::FrameCursor cur{md::itch::Bytes{buf.data(), got}};
    md::itch::Bytes p;
    while (cur.next(p) == md::itch::FrameStatus::Frame) {
    }
    const std::size_t used = cur.offset();
    std::printf("bench: %zu bytes read, %zu used (complete frames)\n", got, used);
    std::uint64_t ref = 0;
    for (const std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{4}}) {
      mc.max_orders_per_shard = a.max_orders;
      pl::MemorySource src{std::span<const std::uint8_t>{buf.data(), used}};
      const std::uint64_t t0 = pl::steady_ns();
      const pl::MarketRun r = pl::run_market(src, mc, n, rc);
      const double secs = static_cast<double>(pl::steady_ns() - t0) / 1e9;
      print_summary(r, secs, n == 0 ? "single" : "shards=" + std::to_string(n));
      if (n == 0) ref = r.summary.summary_hash;
      if (r.summary.summary_hash != ref) {
        std::fprintf(stderr, "FATAL: summary hash differs for N=%zu\n", n);
        return 1;
      }
    }
    std::printf("bench: summary hashes identical for single, N=1, N=2, N=4\n");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "lob_itch_market: %s\n", e.what());
    return 1;
  }
}
