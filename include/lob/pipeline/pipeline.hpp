#pragma once

// Market-data pipeline (L3' stage 2). DESIGN §7.5.
//
//   single:     [ read chunk -> frame -> decode -> book stage -> downstream ]   one thread
//   pipelined:  [ read chunk -> frame -> decode ] --SPSC<Item>--> [ book stage -> downstream ]
//
// Determinism argument: the book stage is the same object running the same
// code in both modes, and it receives the same Items in the same order --
// the producer is the only writer, the SPSC queue is FIFO and lossless (the
// producer spins when it is full; nothing is ever dropped), and the consumer
// is the only reader. Nothing downstream reads a clock to make a decision:
// timestamps feed the latency recorder only. Therefore the event stream and
// the final book are identical byte for byte (tested, not just argued).
//
// Memory is bounded by configuration, independent of input size: one chunk
// buffer (chunk_bytes), one queue (queue_capacity * sizeof(Item)), the book
// (FeedConfig::book) and the latency recorder (capacity per hop). Inputs are
// streamed from a ChunkSource (file descriptor, pipe, memory).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

#include "lob/concurrent/spsc_queue.hpp"
#include "lob/md/feed.hpp"
#include "lob/md/stream.hpp"
#include "lob/pipeline/clock.hpp"

#if defined(__x86_64__)
#include <immintrin.h>
#endif

namespace lob::pipeline {

// ---------------------------------------------------------------- sources

class ChunkSource {
 public:
  virtual ~ChunkSource() = default;
  /// Reads up to `capacity` bytes; returns the count, 0 at end of input, or
  /// a negative value on error.
  virtual std::ptrdiff_t read(std::uint8_t* dst, std::size_t capacity) = 0;

 protected:
  ChunkSource() = default;
  ChunkSource(const ChunkSource&) = default;
  ChunkSource& operator=(const ChunkSource&) = default;
};

/// In-memory source; `max_read` caps each read to exercise chunk boundaries.
class MemorySource final : public ChunkSource {
 public:
  explicit MemorySource(std::span<const std::uint8_t> data, std::size_t max_read = SIZE_MAX)
      : data_(data), max_read_(max_read) {}
  std::ptrdiff_t read(std::uint8_t* dst, std::size_t capacity) override {
    const std::size_t n = std::min({capacity, max_read_, data_.size() - offset_});
    if (n > 0) {
      std::memcpy(dst, data_.data() + offset_, n);
    }
    offset_ += n;
    return static_cast<std::ptrdiff_t>(n);
  }

 private:
  std::span<const std::uint8_t> data_;
  std::size_t max_read_;
  std::size_t offset_{0};
};

/// POSIX file descriptor (a file, or a pipe such as `gzip -dc file |`).
/// Uses read(2) into the caller's fixed buffer: no mmap, so resident memory
/// does not grow with the file size.
class FdSource final : public ChunkSource {
 public:
  explicit FdSource(int fd) : fd_(fd) {}
  std::ptrdiff_t read(std::uint8_t* dst, std::size_t capacity) override;

 private:
  int fd_;
};

// ---------------------------------------------------------------- latency

enum Hop : std::uint8_t { kDecode, kQueue, kBook, kStrategy, kRisk, kEngine, kHopCount };
[[nodiscard]] const char* hop_name(Hop h) noexcept;

/// Preallocated per-hop tick samples. record() never allocates; once a hop's
/// buffer is full further samples are counted as dropped.
class LatencyRecorder {
 public:
  explicit LatencyRecorder(std::size_t capacity_per_hop, std::uint64_t stride = 1);

  [[nodiscard]] bool sampled(std::uint64_t seq) const noexcept { return seq % stride_ == 0; }
  void record(Hop h, std::uint64_t t) noexcept {
    auto& v = samples_[h];
    if (count_[h] < v.size()) {
      v[count_[h]++] = t;
    } else {
      ++dropped_[h];
    }
  }

  struct Summary {
    std::size_t n{0};
    std::uint64_t dropped{0};
    double p50{0}, p99{0}, p999{0}, max{0};  ///< ns, overhead subtracted, clamped at 0
  };
  [[nodiscard]] Summary summarize(Hop h, const ClockCalibration& c) const;
  [[nodiscard]] std::uint64_t stride() const noexcept { return stride_; }

 private:
  std::uint64_t stride_;
  std::vector<std::uint64_t> samples_[kHopCount];
  std::size_t count_[kHopCount]{};
  std::uint64_t dropped_[kHopCount]{};
};

// ---------------------------------------------------------------- stage

/// What crosses the decoder -> book queue.
struct Item {
  md::FeedInput in;
  std::uint64_t t_decode{0};  ///< ticks when framing/decoding of this frame began
  std::uint64_t t_push{0};    ///< ticks just before the push (end of decode)
};
static_assert(std::is_trivially_copyable_v<Item>);

/// Per-thread hooks around each hot loop (used by the zero-allocation test).
struct ThreadHooks {
  void (*begin)() = nullptr;
  void (*end)() = nullptr;
};

/// Feed handler + downstream. Downstream must provide
///   void on_feed(const md::FeedInput&, std::span<const md::FeedEvent>,
///                const md::FeedHandler&, std::uint64_t t_book_done,
///                LatencyRecorder*, bool sampled);
template <class Downstream>
class BookStage {
 public:
  BookStage(const md::FeedConfig& cfg, Downstream& ds, LatencyRecorder* rec = nullptr,
            md::RecoverySource* recovery = nullptr, std::size_t event_capacity = 256)
      : feed_(cfg, recovery), ds_(ds), rec_(rec) {
    events_.reserve(event_capacity);
  }

  /// `t_pop` is the consumer-side timestamp; in single-thread mode it is
  /// `it.t_push` and no queue hop is recorded.
  void consume(const Item& it, std::uint64_t t_pop, bool queued) {
    events_.clear();
    feed_.on_input(it.in, events_);
    const std::uint64_t t_done = ticks();
    const bool s = rec_ != nullptr && rec_->sampled(it.in.seq);
    if (s) {
      rec_->record(kDecode, it.t_push - it.t_decode);
      if (queued) {
        rec_->record(kQueue, t_pop > it.t_push ? t_pop - it.t_push : 0);
      }
      rec_->record(kBook, t_done - t_pop);
    }
    ds_.on_feed(it.in, std::span<const md::FeedEvent>(events_), feed_, t_done, rec_, s);
  }

  [[nodiscard]] const md::FeedHandler& feed() const noexcept { return feed_; }

 private:
  md::FeedHandler feed_;
  Downstream& ds_;
  LatencyRecorder* rec_;
  std::vector<md::FeedEvent> events_;
};

// ---------------------------------------------------------------- drivers

struct RunConfig {
  std::size_t chunk_bytes{1U << 20};       ///< >= 128 KiB (a frame is at most 65 537 bytes)
  std::size_t queue_capacity{1U << 14};    ///< power of two
  std::uint64_t pace_ticks_per_msg{0};     ///< 0 = as fast as possible (saturated)
  /// A spinning side yields its CPU every N failed attempts; 0 = never
  /// (pure spin: lowest hand-off latency, needs a free core per thread).
  std::uint64_t yield_every{64};
  ThreadHooks hooks{};
};

struct RunStats {
  std::uint64_t frames{0};
  bool source_error{false};
  bool frame_error{false};
  std::uint64_t producer_full_spins{0};
  std::uint64_t consumer_empty_spins{0};
};

namespace detail {

inline void relax(std::uint64_t& spins, std::uint64_t yield_every) noexcept {
  ++spins;
#if defined(__x86_64__)
  _mm_pause();
#endif
  if (yield_every != 0 && spins % yield_every == 0) {
    std::this_thread::yield();  // keeps sanitizer / oversubscribed runs moving
  }
}

/// Reads, frames and decodes the whole source; calls emit(const Item&) per
/// frame in order. A truncated tail at end of input, a zero-length prefix,
/// or a read error is reported as one final FrameError item.
template <class Emit>
RunStats produce(ChunkSource& src, const RunConfig& cfg, Emit&& emit) {
  if (cfg.chunk_bytes < (1U << 17)) {
    throw std::invalid_argument("RunConfig::chunk_bytes must be >= 128 KiB");
  }
  RunStats st;
  std::vector<std::uint8_t> buf(cfg.chunk_bytes);
  std::size_t filled = 0;
  std::uint64_t seq = 1;
  bool eof = false;
  if (cfg.hooks.begin != nullptr) cfg.hooks.begin();
  std::uint64_t t_prev = ticks();
  std::uint64_t deadline = t_prev;
  Item item;
  auto sink = [&](const md::FeedInput& in) {
    item.in = in;
    item.t_decode = t_prev;
    item.t_push = ticks();
    emit(static_cast<const Item&>(item));
    ++st.frames;
    if (cfg.pace_ticks_per_msg != 0) {
      deadline += cfg.pace_ticks_per_msg;
      while (ticks() < deadline) {
      }
    }
    t_prev = ticks();
  };
  for (;;) {
    if (!eof && filled < buf.size()) {
      const std::ptrdiff_t r = src.read(buf.data() + filled, buf.size() - filled);
      if (r < 0) {
        st.source_error = true;
        eof = true;
      } else if (r == 0) {
        eof = true;
      } else {
        filled += static_cast<std::size_t>(r);
      }
      t_prev = ticks();  // the read is not part of any message's decode hop
    }
    const md::StreamProgress p = md::decode_frames(md::itch::Bytes{buf.data(), filled}, seq, sink);
    seq = p.next_seq;
    if (p.consumed > 0) {
      std::memmove(buf.data(), buf.data() + p.consumed, filled - p.consumed);
      filled -= p.consumed;
    }
    const bool fatal = p.last == md::itch::FrameStatus::ZeroLength;
    if (fatal || (eof && (filled != 0 || st.source_error))) {
      md::FeedInput fe{};
      fe.seq = seq;
      fe.kind = md::InputKind::FrameError;
      st.frame_error = true;
      sink(fe);
      break;
    }
    if (eof) {
      break;
    }
  }
  if (cfg.hooks.end != nullptr) cfg.hooks.end();
  return st;
}

}  // namespace detail

/// One thread: decode and book stage interleaved (the reference behaviour).
template <class Stage>
RunStats run_single(ChunkSource& src, Stage& stage, const RunConfig& cfg) {
  return detail::produce(src, cfg, [&](const Item& it) { stage.consume(it, it.t_push, false); });
}

/// Two threads: the calling thread decodes, a worker runs the book stage.
template <class Stage>
RunStats run_pipelined(ChunkSource& src, Stage& stage, const RunConfig& cfg) {
  concurrent::SpscQueue<Item> queue{cfg.queue_capacity};
  std::atomic<bool> done{false};
  std::uint64_t empty_spins = 0;
  std::thread consumer([&] {
    if (cfg.hooks.begin != nullptr) cfg.hooks.begin();
    Item it;
    for (;;) {
      if (queue.try_pop(it)) {
        stage.consume(it, ticks(), true);
      } else if (done.load(std::memory_order_acquire)) {
        // acquire pairs with the release store below: every push made before
        // `done` was set is visible, so this drain loses nothing.
        while (queue.try_pop(it)) {
          stage.consume(it, ticks(), true);
        }
        break;
      } else {
        detail::relax(empty_spins, cfg.yield_every);
      }
    }
    if (cfg.hooks.end != nullptr) cfg.hooks.end();
  });
  std::uint64_t full_spins = 0;
  RunStats st;
  try {
    st = detail::produce(src, cfg, [&](const Item& it) {
      while (!queue.try_push(it)) {
        detail::relax(full_spins, cfg.yield_every);
      }
    });
  } catch (...) {
    done.store(true, std::memory_order_release);
    consumer.join();
    throw;
  }
  done.store(true, std::memory_order_release);
  consumer.join();
  st.producer_full_spins = full_spins;
  st.consumer_empty_spins = empty_spins;
  return st;
}

/// Stage-2 downstream: digests the event stream (and optionally keeps its
/// text) without touching the book.
struct EventDigest {
  std::uint64_t hash{md::kHashSeed};
  std::uint64_t events{0};
  std::uint64_t book_updates{0};
  bool keep_text{false};
  std::string text;

  void on_feed(const md::FeedInput&, std::span<const md::FeedEvent> ev, const md::FeedHandler&,
               std::uint64_t, LatencyRecorder*, bool) {
    for (const md::FeedEvent& e : ev) {
      hash = md::hash_event(e, hash);
      ++events;
      book_updates += e.kind == md::FeedEvent::Kind::BookUpdate ? 1U : 0U;
      if (keep_text) {
        md::append_text(e, text);
      }
    }
  }
};

}  // namespace lob::pipeline
