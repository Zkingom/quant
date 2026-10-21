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
