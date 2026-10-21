#pragma once

// Helpers for M5 tests: command streams from the testkit workload profiles,
// and a convenience wrapper that runs a fresh session over a stream.

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "lob/io/recovery.hpp"
#include "lob/io/session.hpp"
#include "lob/io/sink.hpp"
#include "lob/testkit/reference_engine.hpp"
#include "lob/testkit/workload.hpp"

namespace lob::io::test {

/// `n` commands of `profile` (cmd_seq 1..n), text-representable (invalid
/// sides rewritten, see testkit::text_representable). The generator is
/// steered by a reference model of the failure-free run.
inline std::vector<SequencedCommand> generate(const testkit::WorkloadProfile& profile,
                                              std::uint64_t seed, std::size_t n) {
  testkit::ReferenceEngine model{profile.config};
  testkit::WorkloadGenerator generator{profile, seed};
  std::vector<SequencedCommand> out;
  out.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    const Command c = generator.next(static_cast<long>(i), model);
    model.process(c);
    out.push_back(SequencedCommand{.cmd_seq = i + 1, .command = testkit::text_representable(c)});
  }
  return out;
}

inline Recovered fresh(const EngineConfig& config) { return recover("", std::nullopt, config); }

/// Runs every command through a new session writing into `sink`; asserts
/// each one commits. Returns the final committed snapshot.
inline SnapshotFile run_all(const EngineConfig& config, const std::vector<SequencedCommand>& commands,
                            MemorySink& sink, bool durable = false) {
  Session session{fresh(config), sink, SessionOptions{.durable = durable, .prepare_retries = 2}};
  for (const SequencedCommand& c : commands) {
    const SubmitResult r = session.submit(c);
    EXPECT_EQ(r.status, SubmitStatus::Committed) << "cmd_seq " << c.cmd_seq << ' ' << r.detail;
  }
  return session.snapshot_file();
}

}  // namespace lob::io::test
