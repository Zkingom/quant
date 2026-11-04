// lob_itch_icount: fixed, single-threaded workloads for the CI instruction-
// count regression gate (L3' stage 4, scripts/icount_gate.sh, D-051).
//
//   valgrind --tool=cachegrind --cache-sim=no --instr-at-start=no lob_itch_icount <scenario>
//
// Only the measured region is instrumented (cachegrind client requests):
// stream generation and setup are excluded, so the count reflects the code
// under test. Scenarios:
//   decode   frame + decode the whole stream
//   book     single-threaded replay: feed handler + md::Book + event digest
//   trading  replay + market maker + risk + simulated exchange (both fill models)
// Deterministic: fixed seed, no threads, no clock-dependent decisions. The
// printed digest guards against the work being optimised away and lets the
// gate check that the workload itself did not change.

#include <cstdio>
#include <cstring>

#if __has_include(<valgrind/cachegrind.h>)
#include <valgrind/cachegrind.h>
#else
#define CACHEGRIND_START_INSTRUMENTATION
#define CACHEGRIND_STOP_INSTRUMENTATION
#endif

#include "lob/pipeline/pipeline.hpp"
#include "lob/sim/trading.hpp"
#include "lob/testkit/itch_generator.hpp"

namespace pl = lob::pipeline;
namespace md = lob::md;
namespace tk = lob::testkit::itch;

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: lob_itch_icount decode|book|trading\n");
    return 2;
  }
  tk::GeneratorParams gp;
  gp.seed = 1;
  gp.messages = 200'000;
  const tk::Buf stream = tk::generate_stream(gp);
  md::FeedConfig cfg;
  cfg.symbol = md::make_symbol(tk::symbol_of(1));
  cfg.book = md::BookConfig{1U << 14, 1U << 10};
  pl::RunConfig rc;
  rc.chunk_bytes = 1U << 20;
  std::uint64_t digest = 0;

  if (std::strcmp(argv[1], "decode") == 0) {
    std::uint64_t n = 0;
    CACHEGRIND_START_INSTRUMENTATION;
    md::decode_frames(stream, 1, [&](const md::FeedInput& in) { n += static_cast<std::uint64_t>(in.kind) + 1; });
    CACHEGRIND_STOP_INSTRUMENTATION;
    digest = n;
  } else if (std::strcmp(argv[1], "book") == 0) {
    pl::EventDigest d;
    pl::BookStage<pl::EventDigest> stage{cfg, d};
    pl::MemorySource src{stream};
    CACHEGRIND_START_INSTRUMENTATION;
    pl::run_single(src, stage, rc);
    CACHEGRIND_STOP_INSTRUMENTATION;
    digest = d.hash ^ stage.feed().book().state_hash();
  } else if (std::strcmp(argv[1], "trading") == 0) {
    namespace sim = lob::sim;
    namespace st = lob::strategy;
    sim::TradingConfig tc;
    tc.risk.bucket_capacity = 1000;
    tc.risk.refill_per_second = 1'000'000;
    tc.risk.max_rejects_in_window = 1'000'000;
    tc.risk.max_position = 5000;
    tc.timer_interval_ns = 1'000'000;
    st::MarketMaker a{}, b{};
    tc.sim.model = sim::FillModel::Optimistic;
    sim::TradingStage<st::MarketMaker> opt{tc, a};
    tc.sim.model = sim::FillModel::Conservative;
    sim::TradingStage<st::MarketMaker> cons{tc, b};
    sim::Both<decltype(opt), decltype(cons)> both{opt, cons};
    pl::BookStage<decltype(both)> stage{cfg, both};
    pl::MemorySource src{stream};
    CACHEGRIND_START_INSTRUMENTATION;
    pl::run_single(src, stage, rc);
    CACHEGRIND_STOP_INSTRUMENTATION;
    digest = opt.digest().orders ^ opt.digest().fills ^ cons.digest().fills ^ cons.position_hash();
  } else {
    std::fprintf(stderr, "unknown scenario %s\n", argv[1]);
    return 2;
  }
  std::printf("%s digest=0x%016llx\n", argv[1], static_cast<unsigned long long>(digest));
  return 0;
}
