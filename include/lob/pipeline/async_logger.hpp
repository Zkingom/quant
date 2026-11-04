#pragma once

// Asynchronous logger over SPSC queues (L3' stage 2). DESIGN §7.5.
//
// The hot path only enqueues a fixed-size record (format id + up to four
// integer arguments + timestamp): no formatting, no I/O, no allocation, no
// lock. A background thread drains the channels, formats and writes.
//
// Each Channel is one SPSC queue and must be written by exactly one thread
// (one channel per producing thread). When a channel is full the record is
// DROPPED and counted -- the hot path never blocks on logging. Order is
// preserved within a channel; across channels the writer interleaves by
// round-robin, so the output is not globally ordered (each line carries its
// timestamp). Logging never influences the event stream.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "lob/concurrent/spsc_queue.hpp"

namespace lob::pipeline {

struct LogRecord {
  std::uint64_t ts{0};
  std::uint16_t fmt{0};
  std::uint8_t nargs{0};
  std::uint64_t args[4]{};
};
static_assert(std::is_trivially_copyable_v<LogRecord>);

class AsyncLogger {
 public:
  /// `formats[id]` is a text with "{}" placeholders (decimal arguments).
  /// `write` is called from the background thread only, with whole lines.
  AsyncLogger(std::vector<std::string> formats, std::size_t channels, std::size_t capacity_per_channel,
              std::function<void(std::string_view)> write);
  ~AsyncLogger();
  AsyncLogger(const AsyncLogger&) = delete;
  AsyncLogger& operator=(const AsyncLogger&) = delete;

  class Channel {
   public:
    /// Producer side. Never blocks, never allocates. False = dropped (full).
    bool log(std::uint64_t ts, std::uint16_t fmt, std::initializer_list<std::uint64_t> args) noexcept {
      LogRecord r;
      r.ts = ts;
      r.fmt = fmt;
      for (const std::uint64_t a : args) {
        if (r.nargs == 4) break;
        r.args[r.nargs++] = a;
      }
      if (queue_.try_push(r)) {
        return true;
      }
      dropped_.fetch_add(1, std::memory_order_relaxed);  // read only for statistics
      return false;
    }
    [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

   private:
    friend class AsyncLogger;
    explicit Channel(std::size_t capacity) : queue_(capacity) {}
    concurrent::SpscQueue<LogRecord> queue_;
    std::atomic<std::uint64_t> dropped_{0};
  };

  [[nodiscard]] Channel& channel(std::size_t i) { return *channels_[i]; }

  void start();
  /// Drains everything already enqueued, then joins the writer thread.
  void stop();

  [[nodiscard]] std::uint64_t written() const noexcept { return written_.load(std::memory_order_relaxed); }

  /// Formats one record exactly as the writer does (tests).
  [[nodiscard]] std::string format(const LogRecord& r) const;

 private:
  bool drain_once(std::string& buf);

  std::vector<std::string> formats_;
  std::vector<std::unique_ptr<Channel>> channels_;
  std::function<void(std::string_view)> write_;
  std::atomic<bool> stop_{false};
  std::atomic<std::uint64_t> written_{0};
  std::thread thread_;
};

}  // namespace lob::pipeline
