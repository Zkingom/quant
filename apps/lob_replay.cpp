// lob_replay: deterministic replay, recovery and verification (DESIGN.md §6).
//
//   lob_replay run     --commands FILE --journal FILE [--snapshot FILE]
//                      [--snapshot-every N] [--durable] [--echo]
//   lob_replay recover --journal FILE [--snapshot FILE] [--write-snapshot FILE]
//   lob_replay verify  --journal FILE [--commands FILE]
//
// run: recovers from (snapshot +) journal, truncates a torn tail, then submits
// every command of the command file. Commands already committed are skipped
// as duplicates (content-checked), so re-running the same command file after
// a crash continues exactly where the committed history ends. With --echo,
// "ACK <cmd_seq> <first_seq> <last_seq>" (and the events) is printed and
// flushed after each commit; with --durable, after fsync.
//
// Exit codes: 0 ok, 2 usage, 3 malformed input file, 4 journal corrupt,
// 5 recovery refused (snapshot/journal/config disagree), 6 session poisoned,
// 7 command not committed (not applied / refused / gap / cmd_seq reused),
// 8 verify found a divergence, 9 I/O error.

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

#include "lob/engine/matching_engine.hpp"
#include "lob/io/checksum.hpp"
#include "lob/io/journal.hpp"
#include "lob/io/recovery.hpp"
#include "lob/io/session.hpp"
#include "lob/io/sink.hpp"
#include "lob/io/snapshot_file.hpp"
#include "lob/io/text_format.hpp"
#include "lob/io/verify.hpp"

namespace {

using namespace lob;
using namespace lob::io;

struct ExitError {
  int code;
  std::string message;
};

[[noreturn]] void usage(const std::string& why) {
  throw ExitError{2, why + "\nusage:\n"
                           "  lob_replay run --commands FILE --journal FILE [--snapshot FILE] "
                           "[--snapshot-every N] [--durable] [--echo]\n"
                           "  lob_replay recover --journal FILE [--snapshot FILE] [--write-snapshot FILE]\n"
                           "  lob_replay verify --journal FILE [--commands FILE]"};
}

struct Args {
  std::string mode;
  std::map<std::string, std::string> values;
  bool durable = false;
  bool echo = false;

  [[nodiscard]] std::optional<std::string> get(const std::string& key) const {
    const auto it = values.find(key);
    return it == values.end() ? std::nullopt : std::optional<std::string>(it->second);
  }
  [[nodiscard]] std::string need(const std::string& key) const {
    const auto v = get(key);
    if (!v) {
      usage("missing --" + key);
    }
    return *v;
  }
};

Args parse_args(int argc, char** argv) {
  if (argc < 2) {
    usage("missing mode");
  }
  Args a;
  a.mode = argv[1];
  for (int i = 2; i < argc; ++i) {
    const std::string flag = argv[i];
    if (flag == "--durable") {
      a.durable = true;
    } else if (flag == "--echo") {
      a.echo = true;
    } else if (flag.rfind("--", 0) == 0 && i + 1 < argc) {
      a.values[flag.substr(2)] = argv[++i];
    } else {
      usage("bad argument " + flag);
    }
  }
  return a;
}

std::string read_required(const std::string& path) {
  std::string bytes = read_file_if_exists(path);
  if (bytes.empty()) {
    throw ExitError{3, "cannot read " + path + " (missing or empty)"};
  }
  return bytes;
}

std::optional<SnapshotFile> load_snapshot(const std::optional<std::string>& path) {
  if (!path) {
    return std::nullopt;
  }
  const std::string bytes = read_file_if_exists(*path);
  if (bytes.empty()) {
    return std::nullopt;  // no snapshot yet: recover from the journal alone
  }
  try {
    return parse_snapshot(bytes);
  } catch (const ParseError& e) {
    throw ExitError{5, "snapshot " + *path + " unusable: " + e.what()};
  }
}

void print_report(const Recovered& r) {
  const RecoveryReport& rep = r.report;
  std::fprintf(stderr,
               "[recover] journal_batches=%zu applied=%zu snapshot=%s snapshot_cmd=%llu tail=%s "
               "dropped_bytes=%zu valid_bytes=%zu last_cmd=%llu last_seq=%llu orders=%zu state_hash=%s\n",
               rep.batches_in_journal, rep.batches_applied, rep.snapshot_used ? "used" : "none",
               static_cast<unsigned long long>(rep.snapshot_cmd), std::string(to_string(rep.tail)).c_str(),
               rep.dropped_bytes, rep.valid_bytes, static_cast<unsigned long long>(r.last_cmd),
               static_cast<unsigned long long>(r.state.last_seq), r.state.orders.size(),
               to_hex16(state_hash(r.state)).c_str());
}

// ------------------------------------------------------------------------ run

int run(const Args& args) {
  const std::string journal_path = args.need("journal");
  const auto snapshot_path = args.get("snapshot");
  const long snapshot_every = args.get("snapshot-every") ? std::stol(*args.get("snapshot-every")) : 0;

  CommandFile commands;
  try {
    commands = parse_command_file(read_required(args.need("commands")));
  } catch (const ParseError& e) {
    throw ExitError{3, "commands: " + std::string(e.what())};
  }
  const Recovered recovered =
      recover(read_file_if_exists(journal_path), load_snapshot(snapshot_path), commands.config);
  print_report(recovered);

  FileSink sink(journal_path, recovered.report.valid_bytes);
  Session session(recovered, sink, SessionOptions{.durable = args.durable, .prepare_retries = 2});

  std::size_t committed = 0;
  std::size_t duplicates = 0;
  for (const SequencedCommand& command : commands.commands) {
    const SubmitResult r = session.submit(command);
    switch (r.status) {
      case SubmitStatus::Committed:
      case SubmitStatus::CommittedWithResourceFailure:
        ++committed;
        if (args.echo) {  // the acknowledgement: only after the commit point
          std::printf("ACK %llu %llu %llu\n", static_cast<unsigned long long>(command.cmd_seq),
                      static_cast<unsigned long long>(r.first_seq), static_cast<unsigned long long>(r.last_seq));
          for (const Event& e : session.last_events()) {
            std::printf("  %s\n", format_event(e).c_str());
          }
          std::fflush(stdout);
        }
        break;
      case SubmitStatus::Duplicate:
        ++duplicates;
        break;
      case SubmitStatus::Poisoned:
        throw ExitError{6, "cmd_seq " + std::to_string(command.cmd_seq) + ": " + r.detail};
      default:
        throw ExitError{7, "cmd_seq " + std::to_string(command.cmd_seq) + ": " +
                               std::string(to_string(r.status)) + " " + r.detail};
    }
    if (snapshot_path && snapshot_every > 0 && command.cmd_seq % static_cast<CmdSeq>(snapshot_every) == 0) {
      try {
        write_file_atomically(*snapshot_path, encode_snapshot(session.snapshot_file()));
      } catch (const std::exception& e) {
        std::fprintf(stderr, "[warning] snapshot at cmd_seq %llu failed: %s\n",
                     static_cast<unsigned long long>(command.cmd_seq), e.what());
      }
    }
  }
  session.sync();
  if (session.poisoned()) {
    throw ExitError{6, session.poison_reason()};
  }
  const SnapshotFile final_state = session.snapshot_file();
  if (snapshot_path) {
    write_file_atomically(*snapshot_path, encode_snapshot(final_state));
  }
  std::fprintf(stderr,
               "[run] commands=%zu committed=%zu duplicates=%zu last_cmd=%llu last_seq=%llu orders=%zu "
               "state_hash=%s durable=%d\n",
               commands.commands.size(), committed, duplicates,
               static_cast<unsigned long long>(final_state.last_cmd),
               static_cast<unsigned long long>(final_state.state.last_seq), final_state.state.orders.size(),
               to_hex16(state_hash(final_state.state)).c_str(), args.durable ? 1 : 0);
  return 0;
}

// -------------------------------------------------------------------- recover

int recover_mode(const Args& args) {
  const Recovered r = recover(read_required(args.need("journal")), load_snapshot(args.get("snapshot")), std::nullopt);
  print_report(r);
  if (const auto out = args.get("write-snapshot")) {
    write_file_atomically(*out, encode_snapshot(SnapshotFile{.state = r.state, .last_cmd = r.last_cmd}));
  }
  return 0;
}

// --------------------------------------------------------------------- verify

int verify(const Args& args) {
  const JournalScan scan = scan_journal(read_required(args.need("journal")));
  if (!scan.config) {
    throw ExitError{4, "journal has no header"};
  }
  if (const auto path = args.get("commands")) {
    CommandFile commands;
    try {
      commands = parse_command_file(read_required(*path));
    } catch (const ParseError& e) {
      throw ExitError{3, "commands: " + std::string(e.what())};
    }
    if (!(commands.config == *scan.config)) {
      throw ExitError{8, "command file config differs from the journal"};
    }
    for (std::size_t i = 0; i < scan.batches.size(); ++i) {
      if (i >= commands.commands.size() || !(commands.commands[i] == scan.batches[i].command)) {
        throw ExitError{8, "journal batch " + std::to_string(i + 1) + " is not command " +
                               std::to_string(i + 1) + " of the command file"};
      }
    }
  }

  const VerifyResult v = verify_journal(scan);
  if (v.divergence) {
    throw ExitError{8, *v.divergence};
  }
  std::fprintf(stderr,
               "[verify] batches=%zu identical=%zu outcome_from_journal=%zu tail=%s last_seq=%llu state_hash=%s\n",
               v.batches, v.identical, v.outcome_from_journal, std::string(to_string(scan.tail)).c_str(),
               static_cast<unsigned long long>(v.final_state.last_seq), to_hex16(state_hash(v.final_state)).c_str());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Args args = parse_args(argc, argv);
    if (args.mode == "run") {
      return run(args);
    }
    if (args.mode == "recover") {
      return recover_mode(args);
    }
    if (args.mode == "verify") {
      return verify(args);
    }
    usage("unknown mode '" + args.mode + "'");
  } catch (const ExitError& e) {
    std::fprintf(stderr, "lob_replay: %s\n", e.message.c_str());
    return e.code;
  } catch (const JournalCorruption& e) {
    std::fprintf(stderr, "lob_replay: %s\n", e.what());
    return 4;
  } catch (const RecoveryError& e) {
    std::fprintf(stderr, "lob_replay: recovery refused: %s\n", e.what());
    return 5;
  } catch (const ParseError& e) {
    std::fprintf(stderr, "lob_replay: %s\n", e.what());
    return 3;
  } catch (const std::system_error& e) {
    std::fprintf(stderr, "lob_replay: I/O error: %s\n", e.what());
    return 9;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "lob_replay: %s\n", e.what());
    return 9;
  }
}
