// lob_bench: benchmark harness (BENCHMARK.md). Prints CSV on stdout:
//   <scenario>,<metric>,<rep>,<value>
// preceded by "env" lines describing the build.
//
//   lob_bench [--filter SUBSTR] [--reps N] [--engine-commands N]
//             [--durable-commands N] [--ninep-dir DIR] [--quick]

#include <cstdio>
#include <cstdlib>
#include <string>

#include "bench_common.hpp"

namespace lob::bench {
void run_book(const Options& o);
void run_engine(const Options& o);
void run_io(const Options& o);
}  // namespace lob::bench

int main(int argc, char** argv) {
  lob::bench::Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    auto value = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", flag.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (flag == "--filter") {
      o.filter = value();
    } else if (flag == "--reps") {
      o.reps = std::stoi(value());
    } else if (flag == "--engine-commands") {
      o.engine_commands = std::stoul(value());
    } else if (flag == "--durable-commands") {
      o.durable_commands = std::stoul(value());
    } else if (flag == "--ninep-dir") {
      o.ninep_dir = value();
    } else if (flag == "--quick") {
      o.quick = true;
      o.reps = 1;
    } else {
      std::fprintf(stderr, "unknown flag %s\n", flag.c_str());
      return 2;
    }
  }
#ifdef NDEBUG
  const char* ndebug = "1";
#else
  const char* ndebug = "0";
#endif
  std::printf("env,compiler,0,\"%s\"\n", __VERSION__);
  std::printf("env,ndebug,0,%s\n", ndebug);
  std::printf("env,clock_pair_ns,0,%.0f\n", lob::bench::clock_pair_ns());
  lob::bench::run_book(o);
  lob::bench::run_engine(o);
  lob::bench::run_io(o);
  std::printf("env,clock_pair_ns_end,0,%.0f\n", lob::bench::clock_pair_ns());
  std::printf("env,checksum,0,%llu\n", static_cast<unsigned long long>(lob::bench::g_checksum));
  return 0;
}
