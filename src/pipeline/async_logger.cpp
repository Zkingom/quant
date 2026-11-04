#include "lob/pipeline/async_logger.hpp"

#include <stdexcept>

namespace lob::pipeline {

AsyncLogger::AsyncLogger(std::vector<std::string> formats, std::size_t channels,
                         std::size_t capacity_per_channel, std::function<void(std::string_view)> write)
    : formats_(std::move(formats)), write_(std::move(write)) {
  if (channels == 0) {
    throw std::invalid_argument("AsyncLogger needs at least one channel");
  }
  for (std::size_t i = 0; i < channels; ++i) {
    channels_.push_back(std::unique_ptr<Channel>(new Channel(capacity_per_channel)));
  }
}

AsyncLogger::~AsyncLogger() { stop(); }

std::string AsyncLogger::format(const LogRecord& r) const {
  std::string out = std::to_string(r.ts);
  out += ' ';
  const std::string_view f = r.fmt < formats_.size() ? std::string_view(formats_[r.fmt]) : "<bad format {}>";
  std::size_t arg = 0;
  for (std::size_t i = 0; i < f.size(); ++i) {
    if (f[i] == '{' && i + 1 < f.size() && f[i + 1] == '}') {
      out += arg < r.nargs ? std::to_string(r.args[arg]) : std::string("?");
      ++arg;
      ++i;
    } else {
      out += f[i];
    }
  }
  out += '\n';
  return out;
}

bool AsyncLogger::drain_once(std::string& buf) {
  bool any = false;
  LogRecord r;
  for (auto& ch : channels_) {
    // Bounded batch per channel keeps the round-robin fair.
    for (int i = 0; i < 256 && ch->queue_.try_pop(r); ++i) {
      buf += format(r);
      written_.fetch_add(1, std::memory_order_relaxed);
      any = true;
    }
  }
  if (!buf.empty()) {
    write_(buf);
    buf.clear();
  }
  return any;
}

void AsyncLogger::start() {
  if (thread_.joinable()) {
    return;
  }
  stop_.store(false, std::memory_order_relaxed);
  thread_ = std::thread([this] {
    std::string buf;
    for (;;) {
      if (!drain_once(buf)) {
        // acquire pairs with the release in stop(): once we see the flag,
        // every record enqueued before stop() was called is visible, and
        // the final drain below picks it up.
        if (stop_.load(std::memory_order_acquire)) {
          while (drain_once(buf)) {
          }
          return;
        }
        std::this_thread::yield();
      }
    }
  });
}

void AsyncLogger::stop() {
  if (!thread_.joinable()) {
    return;
  }
  stop_.store(true, std::memory_order_release);
  thread_.join();
}

}  // namespace lob::pipeline
