// FileSink and the file helpers on a real (local) file system.

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "lob/io/sink.hpp"

namespace lob::io {
namespace {

std::filesystem::path scratch(const std::string& name) {
  const auto dir = std::filesystem::temp_directory_path() / "lob_file_sink_test";
  std::filesystem::create_directories(dir);
  const auto path = dir / name;
  std::filesystem::remove(path);
  return path;
}

// A restart must cut a torn tail off BEFORE appending; otherwise the next
// batch would follow garbage and the following recovery would see
// corruption in the middle of the journal.
TEST(FileSink, TruncatesToTheKeptLengthBeforeAppending) {
  const auto path = scratch("journal");
  {
    FileSink sink(path.string(), 0);
    sink.write("committed\nTORN-PARTIAL-BATCH-WITHOUT-NEWLINE");
    sink.sync();
  }
  {
    FileSink sink(path.string(), std::string("committed\n").size());
    sink.write("next\n");
    sink.sync();
  }
  EXPECT_EQ(read_file_if_exists(path.string()), "committed\nnext\n");
}

TEST(FileSink, CreatesMissingFilesAndAppends) {
  const auto path = scratch("new");
  FileSink sink(path.string(), 0);
  sink.write("a");
  sink.write("b\n");
  sink.sync();
  EXPECT_EQ(read_file_if_exists(path.string()), "ab\n");
}

TEST(FileHelpers, ReadMissingFileIsEmpty) {
  EXPECT_EQ(read_file_if_exists(scratch("missing").string()), "");
}

TEST(FileHelpers, AtomicWriteReplacesTheFileAndLeavesNoTemporary) {
  const auto path = scratch("snapshot");
  write_file_atomically(path.string(), "first\n");
  write_file_atomically(path.string(), "second version\n");
  EXPECT_EQ(read_file_if_exists(path.string()), "second version\n");
  EXPECT_FALSE(std::filesystem::exists(path.string() + ".tmp"));
}

}  // namespace
}  // namespace lob::io
