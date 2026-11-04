// lob_itch: replay a Nasdaq TotalView-ITCH 5.0 file for one instrument
// (L3' stage 2). Streams the input through a fixed-size chunk buffer, so
// memory does not depend on the file size; reads a file or a pipe:
//
//   gzip -dc data/itch/01302019.NASDAQ_ITCH50.gz | lob_itch --input - --symbol AAPL
//
// Options:
//   --input PATH|-         input file, or - for stdin (required)
//   --symbol SYM           instrument (required)
//   --mode single|pipeline default pipeline (decoder thread -> SPSC -> book thread)
//   --queue N              SPSC capacity, power of two (default 16384)
//   --chunk BYTES          read buffer (default 1048576, >= 131072)
//   --max-orders N         book capacity (default 1048576)
//   --max-levels N         levels per side (default 16384)
//   --samples N            latency samples per hop (default 1048576; 0 = no latency)
//   --stride K             sample every K-th message (default 1; ~400 for a full day)
//   --events PATH          write every feed event as text (large!)
// Prints counters, final state, event-stream and book hashes, throughput,
// the latency waterfall and the peak RSS.

#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>

#include "lob/pipeline/pipeline.hpp"

namespace {

namespace pl = lob::pipeline;
namespace md = lob::md;

struct Args {
  std::string input;
  std::string symbol;
  bool pipelined{true};
  std::size_t queue{1U << 14};
  std::size_t chunk{1U << 20};
  std::size_t max_orders{1U << 20};
  std::size_t max_levels{1U << 14};
  std::size_t samples{1U << 20};
  std::uint64_t stride{1};
  std::string events;
};

[[noreturn]] void usage(const char* msg) {
  std::fprintf(stderr, "lob_itch: %s\nsee the header of apps/lob_itch.cpp for usage\n", msg);
  std::exit(2);
}

Args parse(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string k = argv[i];
    auto val = [&]() -> std::string {
      if (i + 1 >= argc) usage(("missing value for " + k).c_str());
      return argv[++i];
    };
    if (k == "--input") a.input = val();
    else if (k == "--symbol") a.symbol = val();
    else if (k == "--mode") {
      const std::string m = val();
      if (m != "single" && m != "pipeline") usage("--mode must be single or pipeline");
      a.pipelined = m == "pipeline";
    } else if (k == "--queue") a.queue = std::stoull(val());
    else if (k == "--chunk") a.chunk = std::stoull(val());
    else if (k == "--max-orders") a.max_orders = std::stoull(val());
    else if (k == "--max-levels") a.max_levels = std::stoull(val());
    else if (k == "--samples") a.samples = std::stoull(val());
    else if (k == "--stride") a.stride = std::stoull(val());
    else if (k == "--events") a.events = val();
    else usage(("unknown option " + k).c_str());
  }
  if (a.input.empty() || a.symbol.empty()) usage("--input and --symbol are required");
  if (a.symbol.size() > 8) usage("symbol longer than 8 characters");
  return a;
}

struct Downstream {
  pl::EventDigest digest;
  std::ofstream* out{nullptr};
  std::string line;
  void on_feed(const md::FeedInput& in, std::span<const md::FeedEvent> ev, const md::FeedHandler& f,
               std::uint64_t t, pl::LatencyRecorder* r, bool s) {
    digest.on_feed(in, ev, f, t, r, s);
    if (out != nullptr) {
      for (const auto& e : ev) {
        line.clear();
        md::append_text(e, line);
        *out << line;
      }
    }
  }
};

}  // namespace

int main(int argc, char** argv) {
  try {
    const Args a = parse(argc, argv);
    const int fd = a.input == "-" ? 0 : ::open(a.input.c_str(), O_RDONLY);
    if (fd < 0) {
      std::perror(a.input.c_str());
      return 1;
    }
    md::FeedConfig cfg;
    cfg.symbol = md::make_symbol(a.symbol);
    cfg.book = md::BookConfig{a.max_orders, a.max_levels};

    std::unique_ptr<pl::LatencyRecorder> rec;
    if (a.samples > 0) rec = std::make_unique<pl::LatencyRecorder>(a.samples, a.stride);
    Downstream ds;
    std::ofstream events_out;
    if (!a.events.empty()) {
      events_out.open(a.events, std::ios::binary);
      ds.out = &events_out;
    }
    pl::BookStage<Downstream> stage{cfg, ds, rec.get()};
    pl::FdSource src{fd};
    pl::RunConfig rc;
    rc.chunk_bytes = a.chunk;
    rc.queue_capacity = a.queue;

    const pl::ClockCalibration clock = pl::calibrate();
    const std::uint64_t t0 = pl::steady_ns();
    const pl::RunStats st = a.pipelined ? pl::run_pipelined(src, stage, rc) : pl::run_single(src, stage, rc);
    const double secs = static_cast<double>(pl::steady_ns() - t0) / 1e9;
    if (fd != 0) ::close(fd);

    const auto& c = stage.feed().counters();
    std::printf("input=%s symbol=%s mode=%s queue=%zu chunk=%zu\n", a.input.c_str(), a.symbol.c_str(),
                a.pipelined ? "pipeline" : "single", a.queue, a.chunk);
    std::printf("frames=%llu applied=%llu other_stock=%llu skipped=%llu duplicates=%llu gaps=%llu "
                "ignored_not_live=%llu source_error=%d frame_error=%d\n",
                static_cast<unsigned long long>(st.frames), static_cast<unsigned long long>(c.applied),
                static_cast<unsigned long long>(c.other_stock), static_cast<unsigned long long>(c.skipped),
                static_cast<unsigned long long>(c.duplicates), static_cast<unsigned long long>(c.gaps),
                static_cast<unsigned long long>(c.ignored_not_live), st.source_error ? 1 : 0,
                st.frame_error ? 1 : 0);
    std::printf("state=%s events=%llu book_updates=%llu event_hash=0x%016llx book_hash=0x%016llx orders=%zu\n",
                std::string(md::to_string(stage.feed().state())).c_str(),
                static_cast<unsigned long long>(ds.digest.events),
                static_cast<unsigned long long>(ds.digest.book_updates),
                static_cast<unsigned long long>(ds.digest.hash),
                static_cast<unsigned long long>(stage.feed().book().state_hash()),
                stage.feed().book().order_count());
    std::printf("elapsed_s=%.3f frames_per_s=%.0f producer_full_spins=%llu consumer_empty_spins=%llu\n", secs,
                static_cast<double>(st.frames) / secs, static_cast<unsigned long long>(st.producer_full_spins),
                static_cast<unsigned long long>(st.consumer_empty_spins));
    if (rec) {
      std::printf("latency_ns (overhead %.1f ns subtracted; stride %llu)\n",
                  clock.overhead_ticks * clock.ns_per_tick, static_cast<unsigned long long>(a.stride));
      std::printf("| hop | n | p50 | p99 | p99.9 | max | dropped |\n|---|---|---|---|---|---|---|\n");
      for (const pl::Hop h : {pl::kDecode, pl::kQueue, pl::kBook}) {
        const auto s = rec->summarize(h, clock);
        if (s.n == 0) continue;
        std::printf("| %s | %zu | %.0f | %.0f | %.0f | %.0f | %llu |\n", pl::hop_name(h), s.n, s.p50, s.p99,
                    s.p999, s.max, static_cast<unsigned long long>(s.dropped));
      }
    }
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    const double book_mib = static_cast<double>(a.max_orders) * 40.0 / 1048576.0 +
                            static_cast<double>(a.max_levels) * 2.0 * 24.0 / 1048576.0;
    const double bound_mib = static_cast<double>(a.chunk) / 1048576.0 +
                             static_cast<double>(a.queue * sizeof(pl::Item)) / 1048576.0 + book_mib +
                             static_cast<double>(a.samples) * static_cast<double>(pl::kHopCount) * 8.0 / 1048576.0;
    std::printf("peak_rss_kib=%ld configured_bound_mib=%.1f (chunk + queue + book + latency samples; excludes "
                "code, libc, stack)\n",
                ru.ru_maxrss, bound_mib);
    return stage.feed().state() == md::FeedState::Live ? 0 : 3;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "lob_itch: %s\n", e.what());
    return 1;
  }
}
