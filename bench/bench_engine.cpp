// Engine layer (M3): MatchingEngine::process on pre-generated command
// streams of the testkit profiles. The generator (steered by the testkit
// reference engine) runs once per profile before any timing.

#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "bench_common.hpp"
#include "lob/engine/matching_engine.hpp"
#include "lob/io/snapshot_file.hpp"
#include "lob/testkit/reference_engine.hpp"
#include "lob/testkit/workload.hpp"

namespace lob::bench {

std::vector<Command> generate_commands(const testkit::WorkloadProfile& profile, std::size_t n) {
  testkit::ReferenceEngine model{profile.config};
  testkit::WorkloadGenerator generator{profile, 1};
  std::vector<Command> out;
  out.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    out.push_back(generator.next(static_cast<long>(i), model));
    model.process(out.back());
  }
  return out;
}

namespace {

const char* trade_bucket(std::size_t trades) {
  if (trades == 0) {
    return "trades=0";
  }
  if (trades == 1) {
    return "trades=1";
  }
  if (trades <= 4) {
    return "trades=2-4";
  }
  return "trades>=5";
}

void engine_profile(const Options& o, const std::string& profile_name) {
  const std::string name = "engine." + profile_name;
  if (!selected(o, name)) {
    return;
  }
  const testkit::WorkloadProfile& profile = *testkit::find_profile(profile_name);
  const std::size_t n = o.quick ? 2'000 : o.engine_commands;
  const std::vector<Command> commands = generate_commands(profile, n);

  std::optional<MatchingEngine> engine;
  std::vector<Event> events;  // reused across commands, as a long-running caller would
  std::optional<std::uint64_t> state;
  measure_throughput(name, o, n, [&] { engine.emplace(profile.config); }, [&] {
    std::uint64_t total = 0;
    for (const Command& c : commands) {
      events.clear();
      engine->process(c, events);
      total += events.size();
    }
    const std::uint64_t h = io::state_hash(engine->snapshot());
    if (state && *state != h) {
      std::fprintf(stderr, "%s: non-deterministic final state\n", name.c_str());
      std::abort();
    }
    state = h;
    return total ^ h;
  });

  // Latency, overall and by the number of trades the command produced.
  engine.emplace(profile.config);
  std::vector<std::int64_t> all;
  all.reserve(n);
  std::map<std::string, std::vector<std::int64_t>> by_trades;
  std::size_t total_events = 0;
  std::size_t total_trades = 0;
  for (const Command& c : commands) {
    events.clear();
    const auto t0 = now_ns();
    engine->process(c, events);
    const auto t1 = now_ns();
    all.push_back(t1 - t0);
    std::size_t trades = 0;
    for (const Event& e : events) {
      trades += std::holds_alternative<TradeExecuted>(e) ? 1U : 0U;
    }
    total_events += events.size();
    total_trades += trades;
    by_trades[trade_bucket(trades)].push_back(t1 - t0);
  }
  consume(io::state_hash(engine->snapshot()));
  emit(name, "events_per_command", 0, static_cast<double>(total_events) / static_cast<double>(n));
  emit(name, "trades_per_command", 0, static_cast<double>(total_trades) / static_cast<double>(n));
  emit(name, "final_orders", 0, static_cast<double>(engine->book().order_count()));
  emit_latency(name, std::move(all));
  for (auto& [bucket, samples] : by_trades) {
    emit(name + "[" + bucket + "]", "share", 0, static_cast<double>(samples.size()) / static_cast<double>(n));
    emit_latency(name + "[" + bucket + "]", std::move(samples));
  }
}

}  // namespace

void run_engine(const Options& o) {
  for (const char* p : {"mixed", "deep_book", "single_price", "cancel_heavy", "high_density", "thin_book",
                        "extreme_quantity"}) {
    engine_profile(o, p);
  }
}

}  // namespace lob::bench
