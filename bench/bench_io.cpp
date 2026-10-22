// Replay and Session layers (M5). Replay: text parsing, event formatting,
// recovery and verification, all in memory. Session: submit() with a memory
// sink, an ext4 file (buffered and with fsync per batch), and a 9P file with
// fsync — reported separately so that engine, encoding and file-system costs
// can be told apart.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "bench_common.hpp"
#include "lob/io/journal.hpp"
#include "lob/io/recovery.hpp"
#include "lob/io/session.hpp"
#include "lob/io/sink.hpp"
#include "lob/io/snapshot_file.hpp"
#include "lob/io/text_format.hpp"
#include "lob/io/verify.hpp"
#include "lob/testkit/workload.hpp"

namespace lob::bench {

std::vector<Command> generate_commands(const testkit::WorkloadProfile& profile, std::size_t n);

namespace {

std::vector<io::SequencedCommand> sequenced(const std::vector<Command>& commands) {
  std::vector<io::SequencedCommand> out;
  out.reserve(commands.size());
  for (std::size_t i = 0; i < commands.size(); ++i) {
    out.push_back(io::SequencedCommand{.cmd_seq = i + 1, .command = testkit::text_representable(commands[i])});
  }
  return out;
}

/// Runs `commands` through a fresh session on `sink`; optionally records the
/// latency of each submit().
std::uint64_t run_session(const EngineConfig& config, const std::vector<io::SequencedCommand>& commands,
                          io::ByteSink& sink, bool durable, std::vector<std::int64_t>* latency) {
  io::Session session{io::recover("", std::nullopt, config), sink,
                      io::SessionOptions{.durable = durable, .prepare_retries = 2}};
  std::uint64_t events = 0;
  for (const auto& c : commands) {
    const auto t0 = latency != nullptr ? now_ns() : 0;
    const io::SubmitResult r = session.submit(c);
    if (latency != nullptr) {
      latency->push_back(now_ns() - t0);
    }
    if (r.status != io::SubmitStatus::Committed) {
      std::fprintf(stderr, "session benchmark: unexpected status %s\n", std::string(io::to_string(r.status)).c_str());
      std::abort();
    }
    events += session.last_events().size();
  }
  session.sync();
  return events;
}

/// Diagnostic for the maximum submit() latency (M6 notes §6.1, T6-8): one
/// more in-memory session, recording for every submit whether the
/// MemorySink buffer (test-harness storage) reallocated and whether the
/// session's commit-record vector grew (it grows when the number of earlier
/// commands is a power of two). Reports the maximum with and without those
/// submits, so a spike can be attributed to a reallocation or not.
void max_latency_attribution(const EngineConfig& config, const std::vector<io::SequencedCommand>& commands) {
  const std::string name = "replay.session_memory.max_diag";
  io::MemorySink sink;
  io::Session session{io::recover("", std::nullopt, config), sink,
                      io::SessionOptions{.durable = false, .prepare_retries = 2}};
  std::int64_t max_all = 0, max_no_sink = 0, max_neither = 0, max_at_sink = 0;
  std::size_t at = 0, sink_growths = 0;
  for (std::size_t i = 0; i < commands.size(); ++i) {
    const std::size_t capacity = sink.data().capacity();
    const auto t0 = now_ns();
    const io::SubmitResult r = session.submit(commands[i]);
    const std::int64_t dt = now_ns() - t0;
    if (r.status != io::SubmitStatus::Committed) {
      std::abort();
    }
    const bool sink_grew = sink.data().capacity() != capacity;
    const bool records_grew = (i & (i - 1)) == 0;  // i earlier records: 0, 1, 2, 4, ...
    sink_growths += sink_grew ? 1U : 0U;
    if (dt > max_all) {
      max_all = dt;
      at = i;
    }
    if (sink_grew) {
      max_at_sink = std::max(max_at_sink, dt);
    } else {
      max_no_sink = std::max(max_no_sink, dt);
      if (!records_grew) {
        max_neither = std::max(max_neither, dt);
      }
    }
  }
  consume(sink.data().size());
  emit(name, "max_ns", 0, static_cast<double>(max_all));
  emit(name, "max_index", 0, static_cast<double>(at));
  emit(name, "max_at_sink_growth", 0, static_cast<double>(max_at_sink));
  emit(name, "max_without_sink_growth", 0, static_cast<double>(max_no_sink));
  emit(name, "max_without_any_growth", 0, static_cast<double>(max_neither));
  emit(name, "sink_growths", 0, static_cast<double>(sink_growths));
}

void replay(const Options& o, const testkit::WorkloadProfile& profile,
            const std::vector<io::SequencedCommand>& commands) {
  const std::size_t n = commands.size();
  const std::string text = io::format_command_file(profile.config, commands);

  if (selected(o, "replay.parse")) {
    measure_throughput("replay.parse", o, n, [] {}, [&] {
      return static_cast<std::uint64_t>(io::parse_command_file(text).commands.size());
    });
    emit("replay.parse", "bytes_per_command", 0, static_cast<double>(text.size()) / static_cast<double>(n));
  }

  // One reference journal for the format / recover / verify measurements
  // (skipped entirely when none of them is selected, so that a filtered run
  // — e.g. under a profiler — does not execute an extra session).
  if (!any_selected(o, {"replay.format_events", "replay.session_memory", "replay.recover", "replay.verify"})) {
    return;
  }
  io::MemorySink journal;
  consume(run_session(profile.config, commands, journal, false, nullptr));
  const io::JournalScan scan = io::scan_journal(journal.data());
  std::vector<Event> all_events;
  for (const auto& b : scan.batches) {
    all_events.insert(all_events.end(), b.events.begin(), b.events.end());
  }

  if (selected(o, "replay.format_events")) {
    measure_throughput("replay.format_events", o, all_events.size(), [] {}, [&] {
      std::uint64_t bytes = 0;
      for (const Event& e : all_events) {
        bytes += io::format_event(e).size();
      }
      return bytes;
    });
  }
  if (selected(o, "replay.session_memory")) {
    std::optional<io::MemorySink> sink;
    measure_throughput("replay.session_memory", o, n, [&] { sink.emplace(); },
                       [&] { return run_session(profile.config, commands, *sink, false, nullptr); });
    io::MemorySink s;
    std::vector<std::int64_t> ns;
    ns.reserve(n);
    consume(run_session(profile.config, commands, s, false, &ns));
    emit_latency("replay.session_memory", std::move(ns));
    emit("replay.session_memory", "journal_bytes_per_command", 0,
         static_cast<double>(journal.data().size()) / static_cast<double>(n));
    max_latency_attribution(profile.config, commands);
  }
  if (selected(o, "replay.recover")) {
    measure_throughput("replay.recover", o, n, [] {}, [&] {
      return static_cast<std::uint64_t>(io::recover(journal.data(), std::nullopt, std::nullopt).last_cmd);
    });
  }
  if (selected(o, "replay.verify")) {
    measure_throughput("replay.verify", o, n, [] {}, [&] {
      const io::VerifyResult v = io::verify_journal(scan);
      if (v.divergence) {
        std::abort();
      }
      return static_cast<std::uint64_t>(v.identical);
    });
  }
}

void session_file(const Options& o, const std::string& name, const std::string& path,
                  const EngineConfig& config, const std::vector<io::SequencedCommand>& commands, bool durable) {
  if (!selected(o, name)) {
    return;
  }
  const std::size_t n = commands.size();
  std::optional<io::FileSink> sink;
  measure_throughput(name, o, n, [&] { sink.emplace(path, 0); },
                     [&] { return run_session(config, commands, *sink, durable, nullptr); });
  sink.emplace(path, 0);
  std::vector<std::int64_t> ns;
  ns.reserve(n);
  consume(run_session(config, commands, *sink, durable, &ns));
  emit_latency(name, std::move(ns));
  sink.reset();
  std::filesystem::remove(path);
}

}  // namespace

void run_io(const Options& o) {
  if (!any_selected(o, {"replay.parse", "replay.format_events", "replay.session_memory", "replay.recover",
                        "replay.verify", "session.memory", "session.ext4_buffered", "session.ext4_fsync",
                        "session.9p_fsync"})) {
    return;  // nothing selected: do not even generate the workload
  }
  const testkit::WorkloadProfile& profile = *testkit::find_profile("mixed");
  const std::size_t n = o.quick ? 2'000 : o.engine_commands;
  const auto commands = sequenced(generate_commands(profile, n));
  replay(o, profile, commands);

  if (selected(o, "session.memory")) {
    std::optional<io::MemorySink> sink;
    measure_throughput("session.memory", o, n, [&] { sink.emplace(); },
                       [&] { return run_session(profile.config, commands, *sink, false, nullptr); });
  }
  const std::vector<io::SequencedCommand> short_run(
      commands.begin(), commands.begin() + static_cast<std::ptrdiff_t>(std::min(n, o.quick ? 200 : o.durable_commands)));
  session_file(o, "session.ext4_buffered", "/tmp/lob_bench.journal", profile.config, commands, false);
  session_file(o, "session.ext4_fsync", "/tmp/lob_bench.journal", profile.config, short_run, true);
  if (!o.ninep_dir.empty()) {
    session_file(o, "session.9p_fsync", o.ninep_dir + "/lob_bench.journal", profile.config, short_run, true);
  }
}

}  // namespace lob::bench
