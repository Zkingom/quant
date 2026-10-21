#pragma once

// Where journal bytes go. write() and sync() report failure by throwing
// (std::system_error for files); the session treats any exception from
// either as "not committed" (DESIGN.md §6.4). Tests implement failing sinks
// against this interface; production code never depends on test code.

#include <cstddef>
#include <string>
#include <string_view>

namespace lob::io {

class ByteSink {
 public:
  virtual ~ByteSink() = default;
  /// Appends all of `bytes` or throws. A throwing write may have written a
  /// prefix (as a real file can after a crash or a full disk).
  virtual void write(std::string_view bytes) = 0;
  /// Makes everything written so far durable (fsync for files) or throws.
  virtual void sync() = 0;
};

/// In-memory sink (replay, verification and tests). `synced_size()` marks
/// what a crash would be guaranteed to preserve.
class MemorySink final : public ByteSink {
 public:
  void write(std::string_view bytes) override { data_.append(bytes); }
  void sync() override { synced_ = data_.size(); }
  [[nodiscard]] const std::string& data() const noexcept { return data_; }
  [[nodiscard]] std::size_t synced_size() const noexcept { return synced_; }

 private:
  std::string data_;
  std::size_t synced_ = 0;
};

/// Appending file sink (POSIX). The constructor opens (creating if needed)
/// and truncates the file to `keep_bytes` — the recovered valid length, so
/// that new batches never follow a torn tail. sync() is fsync(2).
class FileSink final : public ByteSink {
 public:
  FileSink(const std::string& path, std::size_t keep_bytes);
  ~FileSink() override;
  FileSink(const FileSink&) = delete;
  FileSink& operator=(const FileSink&) = delete;

  void write(std::string_view bytes) override;
  void sync() override;

 private:
  std::string path_;
  int fd_ = -1;
};

/// Reads a whole file; returns an empty string if it does not exist.
std::string read_file_if_exists(const std::string& path);

/// Writes `bytes` to `path` via a temporary file in the same directory:
/// write, fsync, rename over `path`, fsync the directory. Readers see either
/// the old or the new complete file, never a partial one (on file systems
/// that implement rename atomically and honour fsync; DESIGN.md §6.6).
void write_file_atomically(const std::string& path, std::string_view bytes);

}  // namespace lob::io
