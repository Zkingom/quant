// Golden tests (DESIGN.md §6.2 Q8, C6): for each scenario under tests/golden,
// running its command file through a fresh session must reproduce the
// committed journal and final snapshot BYTE FOR BYTE, on every preset and
// compiler. Re-execution (verify) must agree with the journal.
//
// To regenerate after an intentional format/semantics change:
//   LOB_UPDATE_GOLDEN=1 ./build/gcc-debug/tests/lob_io_tests --gtest_filter='Golden*'
// then review the diff of tests/golden/ by hand before committing.

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "io_test_util.hpp"
#include "lob/io/journal.hpp"
#include "lob/io/snapshot_file.hpp"
#include "lob/io/text_format.hpp"
#include "lob/io/verify.hpp"

#ifndef LOB_GOLDEN_DIR
#error "LOB_GOLDEN_DIR must be defined by the build"
#endif

namespace lob::io {
namespace {

const std::string kDir = LOB_GOLDEN_DIR;

bool update_mode() {
  const char* v = std::getenv("LOB_UPDATE_GOLDEN");
  return v != nullptr && std::string(v) == "1";
}

void write_file(const std::string& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << bytes;
  ASSERT_TRUE(out.good()) << path;
}

std::string seeded_commands() {
  const auto& profile = *testkit::find_profile("mixed");
  return format_command_file(profile.config, test::generate(profile, 1, 2'000));
}

class Golden : public ::testing::TestWithParam<std::string> {};

TEST_P(Golden, JournalAndSnapshotAreReproducedByteForByte) {
  const std::string name = GetParam();
  const std::string base = kDir + "/" + name;
  if (name == "seeded_mixed_2000") {
    // The seeded command file itself is pinned: the generator must still
    // produce it (determinism of the test input).
    if (update_mode()) {
      write_file(base + ".cmd", seeded_commands());
    }
    ASSERT_EQ(read_file_if_exists(base + ".cmd"), seeded_commands());
  }
  const CommandFile commands = parse_command_file(read_file_if_exists(base + ".cmd"));
  MemorySink sink;
  const SnapshotFile final_state = test::run_all(commands.config, commands.commands, sink);
  const std::string snapshot = encode_snapshot(final_state);
  if (update_mode()) {
    write_file(base + ".journal", sink.data());
    write_file(base + ".snap", snapshot);
  }
  EXPECT_EQ(sink.data(), read_file_if_exists(base + ".journal")) << base << ".journal differs";
  EXPECT_EQ(snapshot, read_file_if_exists(base + ".snap")) << base << ".snap differs";

  const JournalScan scan = scan_journal(sink.data());
  const VerifyResult v = verify_journal(scan);
  EXPECT_EQ(v.divergence, std::nullopt);
  EXPECT_EQ(v.identical, commands.commands.size());
  EXPECT_EQ(recover(sink.data(), std::nullopt, std::nullopt).state, final_state.state);
}

INSTANTIATE_TEST_SUITE_P(Scenarios, Golden,
                         ::testing::Values("basic", "priority", "sweep_partial", "rejections", "id_reuse",
                                           "seeded_mixed_2000"));

}  // namespace
}  // namespace lob::io
