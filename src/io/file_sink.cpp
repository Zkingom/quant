#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include "lob/io/sink.hpp"

namespace lob::io {

namespace {

[[noreturn]] void throw_errno(const std::string& what) {
  throw std::system_error(errno, std::generic_category(), what);
}

void write_all(int fd, std::string_view bytes, const std::string& path) {
  while (!bytes.empty()) {
    const ssize_t n = ::write(fd, bytes.data(), bytes.size());
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw_errno("write " + path);
    }
    bytes.remove_prefix(static_cast<std::size_t>(n));
  }
}

void fsync_or_throw(int fd, const std::string& path) {
  if (::fsync(fd) != 0) {
    throw_errno("fsync " + path);
  }
}

}  // namespace

FileSink::FileSink(const std::string& path, std::size_t keep_bytes) : path_(path) {
  fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
  if (fd_ < 0) {
    throw_errno("open " + path);
  }
  if (::ftruncate(fd_, static_cast<off_t>(keep_bytes)) != 0 ||
      ::lseek(fd_, static_cast<off_t>(keep_bytes), SEEK_SET) < 0) {
    const int saved = errno;
    ::close(fd_);
    fd_ = -1;
    throw std::system_error(saved, std::generic_category(), "truncate " + path);
  }
}

FileSink::~FileSink() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

void FileSink::write(std::string_view bytes) { write_all(fd_, bytes, path_); }

void FileSink::sync() { fsync_or_throw(fd_, path_); }

std::string read_file_if_exists(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }
  std::ostringstream content;
  content << in.rdbuf();
  if (in.bad()) {
    throw std::system_error(EIO, std::generic_category(), "read " + path);
  }
  return content.str();
}

void write_file_atomically(const std::string& path, std::string_view bytes) {
  const std::filesystem::path target(path);
  const std::filesystem::path temp = target.string() + ".tmp";
  const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) {
    throw_errno("open " + temp.string());
  }
  try {
    write_all(fd, bytes, temp.string());
    fsync_or_throw(fd, temp.string());
  } catch (...) {
    ::close(fd);
    throw;
  }
  ::close(fd);
  std::filesystem::rename(temp, target);  // throws filesystem_error on failure
  const std::filesystem::path dir = target.has_parent_path() ? target.parent_path() : ".";
  const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dfd >= 0) {
    const int rc = ::fsync(dfd);
    ::close(dfd);
    if (rc != 0) {
      throw_errno("fsync directory " + dir.string());
    }
  }
}

}  // namespace lob::io
