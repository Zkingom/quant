#pragma once

// Market-wide order-book reconstruction (D-056): every instrument of the
// stream, each with its own md::Book and FeedHandler (trading phase, trading
// state, resume grace, Stale -- all per instrument), books sharing one
// OrderPool per shard (order refs are unique market-wide for the day).
//
//   Router (one, in stream order)        Shard (one or N)
//   - sequence contiguity, timestamp     - handlers indexed by stock locate,
//     monotonicity, frame / decode         created at the instrument's
//     errors: the stream-level checks      Stock Directory
//   - route by the message's locate:     - per-instrument event digest
//     stock messages to shard(locate),
//     System Events and stream faults
//     broadcast to every shard
//
// Single-threaded mode = Router + 1 Shard called inline. Sharded mode =
// Router on the decoder thread, N SPSC queues, N worker threads (shard k
// owns the locates with locate % N == k). Every instrument receives the
// same inputs in the same order in both modes (its own messages, and the
// broadcasts in stream order), so its event stream and book are identical;
// the market summary hash combines the per-instrument digests with an
// order-independent sum.

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "lob/md/book.hpp"
#include "lob/md/feed.hpp"

namespace lob::md {

inline constexpr std::size_t kMaxLocates = 65536;
inline constexpr std::size_t kMaxRemembered = 256;  ///< broadcasts replayed to late instruments

struct MarketConfig {
  std::size_t max_orders_per_shard{1U << 22};  ///< OrderPool capacity of each shard
  std::size_t max_levels_per_side{1U << 14};
  std::size_t initial_levels_per_side{16};
  std::uint64_t resume_grace_ns{1'000'000'000ULL};
  std::uint64_t resume_grace_messages{10'000};
};

/// What the router hands to a shard.
struct Routed {
  enum class Kind : std::uint8_t { Stock, Broadcast, Fault };
  FeedInput in;
  Kind kind{Kind::Stock};
  AlertReason fault{AlertReason::None};
};
static_assert(std::is_trivially_copyable_v<Routed>);

struct RouterCounters {
  std::uint64_t frames{0};
  std::uint64_t routed{0};
  std::uint64_t broadcasts{0};
  std::uint64_t faults{0};
  std::uint64_t skipped{0};  ///< non-subset types (not routed)
};

/// Stream-level checks and routing. emit(shard_index, const Routed&).
class Router {
 public:
  explicit Router(std::size_t shards) : shards_(shards == 0 ? 1 : shards) {}

  template <class Emit>
  void on_input(const FeedInput& in, Emit&& emit) {
    ++c_.frames;
    Routed r{in, Routed::Kind::Stock, AlertReason::None};
    const bool gap = in.seq != expected_;
    expected_ = in.seq + 1;
    if (gap || in.kind == InputKind::FrameError || in.kind == InputKind::DecodeError) {
      r.kind = Routed::Kind::Fault;
      r.fault = gap ? AlertReason::SequenceGap
                    : (in.kind == InputKind::FrameError ? AlertReason::FrameError : AlertReason::DecodeError);
      broadcast(r, emit);
      if (gap && in.kind == InputKind::Decoded) {
        route(in, emit);  // the message itself is still delivered (to a Stale handler)
      }
      return;
    }
    if (in.kind == InputKind::Skipped) {
      ++c_.skipped;
      return;
    }
    const auto& h = itch::header_of(in.msg);
    if (h.timestamp < last_ts_) {
      r.kind = Routed::Kind::Fault;
      r.fault = AlertReason::TimestampRegression;
      broadcast(r, emit);
      return;
    }
    last_ts_ = h.timestamp;
    route(in, emit);
  }

  [[nodiscard]] std::size_t shard_of(std::uint16_t locate) const noexcept { return locate % shards_; }
  [[nodiscard]] std::size_t shards() const noexcept { return shards_; }
  [[nodiscard]] const RouterCounters& counters() const noexcept { return c_; }

 private:
  template <class Emit>
  void broadcast(const Routed& r, Emit& emit) {
    ++c_.faults;
    for (std::size_t k = 0; k < shards_; ++k) emit(k, r);
  }
  template <class Emit>
  void route(const FeedInput& in, Emit& emit) {
    if (std::holds_alternative<itch::SystemEvent>(in.msg)) {
      ++c_.broadcasts;
      for (std::size_t k = 0; k < shards_; ++k) emit(k, Routed{in, Routed::Kind::Broadcast, AlertReason::None});
      return;
    }
    ++c_.routed;
    emit(shard_of(itch::header_of(in.msg).stock_locate), Routed{in, Routed::Kind::Stock, AlertReason::None});
  }

  std::size_t shards_;
  std::uint64_t expected_{1};
  std::uint64_t last_ts_{0};
  RouterCounters c_{};
};

struct StockResult {
  std::uint16_t locate{0};
  std::string symbol;
  FeedState state{FeedState::Uninitialized};
  AlertReason stale_reason{AlertReason::None};  ///< reason of the transition to Stale, if any
  std::uint64_t events{0};
  std::uint64_t event_hash{kHashSeed};
  std::uint64_t book_hash{0};
  std::size_t orders{0};
  std::size_t peak_levels{0};  ///< max over both sides of the book's peak level count
  FeedCounters counters{};
};

/// One shard: the handlers of its instruments and their digests.
class Shard {
 public:
  Shard(const MarketConfig& cfg, std::size_t index);

  void apply(const Routed& r);

  [[nodiscard]] const OrderPool& pool() const noexcept { return *pool_; }
  [[nodiscard]] std::size_t index() const noexcept { return index_; }
  [[nodiscard]] std::uint64_t unknown_locate() const noexcept { return unknown_locate_; }
  [[nodiscard]] std::size_t instruments() const noexcept { return order_.size(); }
  /// Results of this shard's instruments, ascending locate.
  [[nodiscard]] std::vector<StockResult> results() const;

 private:
  struct Slot {
    std::unique_ptr<FeedHandler> h;
    std::uint64_t events{0};
    std::uint64_t hash{kHashSeed};
    AlertReason stale_reason{AlertReason::None};
  };
  void digest(Slot& s, std::uint16_t locate);

  MarketConfig cfg_;
  std::size_t index_;
  std::unique_ptr<OrderPool> pool_;
  std::vector<Slot> slots_;            // by locate (kMaxLocates)
  std::vector<std::uint16_t> order_;   // locates with a handler, in creation order
  std::vector<FeedEvent> events_;
  std::vector<Routed> broadcasts_;     // replayed to instruments created later (bounded, see .cpp)
  std::uint64_t unknown_locate_{0};
};

struct MarketSummary {
  std::vector<StockResult> stocks;  ///< ascending locate
  std::uint64_t summary_hash{0};    ///< order-independent combination of every instrument's digest
  std::size_t live{0}, stale{0}, uninitialized{0}, other{0};
  std::map<std::string, std::size_t> stale_reasons;
  std::size_t pool_peak_sum{0};     ///< sum of the shards' peaks (= exact peak with one shard)
  std::uint64_t unknown_locate{0};
};

/// Per-instrument digest folded into the summary: FNV over
/// (locate, state, event_hash, book_hash), summed mod 2^64.
[[nodiscard]] MarketSummary summarize(const std::vector<std::unique_ptr<Shard>>& shards);

}  // namespace lob::md
