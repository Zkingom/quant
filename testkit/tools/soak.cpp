// lob_soak: long-run deterministic differential testing.
//
//   lob_soak --list
//   lob_soak --profile NAME|all [--seed S | --seeds A-B] [--commands N]
//            [--check-every K] [--fault-per-mille F] [--require-targets]
//
// Runs MatchingEngine against ReferenceEngine + EventAuditor (see
// lob/testkit/differential.hpp) for every (profile, seed). Prints one [run]
// line per seed and, per profile, the merged coverage, the boundary targets
// and the process resource usage. On the first failure it prints the
// violation and a [repro] command line, and exits 1. --check-every does not
// change the command stream (full checks consume no randomness), so a
// failure found with a large K reproduces exactly with --check-every 1.

#include <sys/resource.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "lob/testkit/alloc_fault.hpp"
#include "lob/testkit/differential.hpp"
#include "lob/testkit/workload.hpp"

namespace {

using lob::testkit::Coverage;
using lob::testkit::RunOptions;
using lob::testkit::RunReport;
using lob::testkit::WorkloadProfile;

struct Args {
  std::string profile = "all";
  std::uint64_t seed_first = 1;
  std::uint64_t seed_last = 1;
  long commands = 10'000;
  long check_every = 1'000;
  int fault_per_mille = 0;
  bool require_targets = false;
  bool list = false;
};

[[noreturn]] void usage(const char* why) {
  std::fprintf(stderr,
               "lob_soak: %s\nusage: lob_soak --list | --profile NAME|all [--seed S | --seeds A-B] "
               "[--commands N] [--check-every K] [--fault-per-mille F] [--require-targets]\n",
               why);
  std::exit(2);
}

Args parse(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    auto value = [&]() -> std::string {
      if (i + 1 >= argc) {
        usage(("missing value for " + flag).c_str());
      }
      return argv[++i];
    };
    if (flag == "--list") {
      a.list = true;
    } else if (flag == "--profile") {
      a.profile = value();
    } else if (flag == "--seed") {
      a.seed_first = a.seed_last = std::stoull(value());
    } else if (flag == "--seeds") {
      const std::string v = value();
      const auto dash = v.find('-');
      if (dash == std::string::npos) {
        usage("--seeds expects A-B");
      }
      a.seed_first = std::stoull(v.substr(0, dash));
      a.seed_last = std::stoull(v.substr(dash + 1));
    } else if (flag == "--commands") {
      a.commands = std::stol(value());
    } else if (flag == "--check-every") {
      a.check_every = std::stol(value());
    } else if (flag == "--fault-per-mille") {
      a.fault_per_mille = std::stoi(value());
    } else if (flag == "--require-targets") {
      a.require_targets = true;
    } else {
      usage(("unknown flag " + flag).c_str());
    }
  }
  if (a.commands < 1 || a.check_every < 1 || a.seed_last < a.seed_first || a.fault_per_mille < 0 ||
      a.fault_per_mille > 1'000) {
    usage("invalid argument value");
  }
  return a;
}

void print_resources() {
  rusage ru{};
  getrusage(RUSAGE_SELF, &ru);
  std::printf("[resources] max_rss_kib=%ld user_s=%.2f sys_s=%.2f\n", ru.ru_maxrss,
              static_cast<double>(ru.ru_utime.tv_sec) + static_cast<double>(ru.ru_utime.tv_usec) / 1e6,
              static_cast<double>(ru.ru_stime.tv_sec) + static_cast<double>(ru.ru_stime.tv_usec) / 1e6);
}

}  // namespace

int main(int argc, char** argv) {
  const Args args = parse(argc, argv);
  if (args.list) {
    for (const WorkloadProfile& p : lob::testkit::standard_profiles()) {
      std::printf("%-17s %s (targets from %ld commands/seed)\n", p.name.c_str(), p.purpose.c_str(),
                  p.target_commands);
    }
    return 0;
  }

  std::vector<const WorkloadProfile*> profiles;
  if (args.profile == "all") {
    for (const WorkloadProfile& p : lob::testkit::standard_profiles()) {
      profiles.push_back(&p);
    }
  } else if (const WorkloadProfile* p = lob::testkit::find_profile(args.profile)) {
    profiles.push_back(p);
  } else {
    usage(("unknown profile " + args.profile).c_str());
  }

  lob::testkit::alloc::Injector injector;
  bool targets_ok = true;
  for (const WorkloadProfile* profile : profiles) {
    Coverage merged;
    double seconds = 0;
    for (std::uint64_t seed = args.seed_first; seed <= args.seed_last; ++seed) {
      RunOptions options;
      options.seed = seed;
      options.commands = args.commands;
      options.full_check_every = args.check_every;
      options.fault_per_mille = args.fault_per_mille;
      options.injector = &injector;
      const RunReport report = lob::testkit::run_differential(*profile, options);
      std::printf("[run] profile=%s seed=%llu commands=%ld check_every=%ld fault_per_mille=%d "
                  "ok=%d seconds=%.3f\n",
                  profile->name.c_str(), static_cast<unsigned long long>(seed), args.commands,
                  args.check_every, args.fault_per_mille, report.ok ? 1 : 0, report.seconds);
      if (!report.ok) {
        std::printf("[failure] %s\n", report.failure.c_str());
        std::printf("[repro] lob_soak --profile %s --seed %llu --commands %ld --check-every 1 "
                    "--fault-per-mille %d\n",
                    profile->name.c_str(), static_cast<unsigned long long>(seed),
                    report.failed_step + 1, args.fault_per_mille);
        print_resources();
        return 1;
      }
      merged.merge(report.coverage);
      seconds += report.seconds;
    }
    std::printf("== profile %s: %s\n", profile->name.c_str(), profile->purpose.c_str());
    std::printf("%s", merged.summary().c_str());
    std::printf("[time] seconds=%.3f commands_per_second(harness incl. reference+audit)=%.0f\n",
                seconds, static_cast<double>(merged.commands + merged.faults_prepare +
                                             merged.faults_residual) /
                             (seconds > 0 ? seconds : 1));
    if (args.commands >= profile->target_commands) {
      const auto unmet = lob::testkit::unmet_targets(*profile, merged);
      if (unmet.empty()) {
        std::printf("[targets] all met\n");
      } else {
        targets_ok = false;
        for (const std::string& t : unmet) {
          std::printf("[targets] UNMET: %s\n", t.c_str());
        }
      }
    } else {
      std::printf("[targets] not checked (%ld < %ld commands/seed)\n", args.commands,
                  profile->target_commands);
    }
  }
  print_resources();
  return (args.require_targets && !targets_ok) ? 1 : 0;
}
