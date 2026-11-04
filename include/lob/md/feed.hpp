#pragma once

// Feed handler: per-instrument filter, gap detection, recovery state machine
// and book publication (L3' 1d). DESIGN §7.4.
//
// Input is a stream of FeedInput -- one per frame of the source, in source
// order, each carrying a sequence number:
//   - file replay: the 1-based frame index in the file (contiguous by
//     construction, so a gap can only come from a lossy transport, e.g. a
//     dropped queue element between decoder and book thread);
//   - MoldUDP64: packet sequence number + index within the packet.
//
// States (per instrument; the handler owns one instrument):
//   Uninitialized --(Stock Directory for our symbol)--> Live
//   Live --(sequence gap)--> Recovering --(retransmission fills it)--> Live
//                                      \--(unavailable / fails)-------> Stale
//   Live --(timestamp regression | book rejected a message |
//           undecodable or unframeable input)------------------------> Stale
//   Stale --(resync_from_snapshot)---------------------------------> Live
// While not Live the handler publishes no BookUpdate: downstream sees only
// the StateChange / Alert events and must treat the book as unusable. A book
// that rejected a message is never used again until a snapshot replaces it.
//
// Duplicate sequence numbers (seq < expected) are dropped and counted: on
// MoldUDP64 A/B feeds they are routine, not an error. Timestamps must be
// non-decreasing over the whole stream (every instrument); a forward jump is
// not treated as loss -- quiet periods are normal, and sequence numbers are
// the loss detector.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "lob/md/book.hpp"
#include "lob/md/itch.hpp"

namespace lob::md {

enum class InputKind : std::uint8_t {
  Decoded,      ///< `msg` valid
  Skipped,      ///< valid ITCH type outside the subset
  DecodeError,  ///< `error` holds the itch::DecodeStatus
  FrameError,   ///< the byte stream could not be framed (truncated / zero length)
};

struct FeedInput {
  std::uint64_t seq{0};
  InputKind kind{InputKind::Decoded};
  std::uint8_t error{0};
  itch::Message msg{};
};
static_assert(std::is_trivially_copyable_v<FeedInput>);

enum class FeedState : std::uint8_t { Uninitialized, Live, Recovering, Stale };

enum class AlertReason : std::uint8_t {
  None,
  SequenceGap,
  GapRecovered,
  RecoveryFailed,
  TimestampRegression,
  BookRejected,      ///< `detail` holds the BookStatus
  SymbolMismatch,    ///< add order for our locate carries another symbol
  DecodeError,       ///< `detail` holds the itch::DecodeStatus
  FrameError,
  SnapshotLoaded,
  SnapshotRejected,  ///< `detail` holds the BookStatus of the first bad order
};

[[nodiscard]] std::string_view to_string(FeedState s) noexcept;
[[nodiscard]] std::string_view to_string(AlertReason r) noexcept;

/// Output event. Trivially copyable (crosses SPSC queues downstream); hash or
/// print it with the helpers below, never by raw bytes (padding).
struct FeedEvent {
  enum class Kind : std::uint8_t { BookUpdate, StateChange, Alert, System };
  Kind kind{Kind::BookUpdate};
  FeedState state{FeedState::Uninitialized};  ///< state after this event
  AlertReason reason{AlertReason::None};
  std::uint8_t detail{0};
  char msg_type{0};            ///< ITCH type that caused the event (0 if none)
  std::uint64_t seq{0};
  std::uint64_t timestamp{0};  ///< ITCH ns since midnight
  std::uint64_t order_ref{0};  ///< BookUpdate: order affected (new ref for replace)
  std::uint32_t bid_price{0};  ///< BBO after the event; 0 = side empty
  std::uint32_t ask_price{0};
  std::uint64_t bid_shares{0};
  std::uint64_t ask_shares{0};
  std::uint64_t gap_first{0};  ///< SequenceGap: first missing seq
  std::uint64_t gap_last{0};   ///< SequenceGap: last missing seq
  // BookUpdate only (stage 3: the fill simulator needs what happened, not
  // just the resulting BBO). Looked up BEFORE the message is applied.
  //   A/F: the new order          E/C: the resting order executed (its book
  //   price; `shares` = executed)  X: cancelled shares   D: shares removed
  //   U: the new order, and prev_* = the replaced order
  char side{0};                ///< 'B' / 'S'
  std::uint32_t price{0};
  std::uint32_t shares{0};
  std::uint64_t prev_ref{0};
  std::uint32_t prev_price{0};
  std::uint32_t prev_shares{0};
};
static_assert(std::is_trivially_copyable_v<FeedEvent>);

/// Canonical single-line text of an event (tests, golden files, logs).
void append_text(const FeedEvent& e, std::string& out);
/// FNV-1a 64 over the event's fields (not its bytes).
[[nodiscard]] std::uint64_t hash_event(const FeedEvent& e, std::uint64_t h) noexcept;
inline constexpr std::uint64_t kHashSeed = 0xcbf29ce484222325ULL;

/// Retransmission source for gap recovery (MoldUDP64 rewinder in a real
/// system; in tests, a copy of the stream). Not on the hot path.
class RecoverySource {
 public:
  virtual ~RecoverySource() = default;
  /// Append the inputs with seq in [first, last], in order. Return false if
  /// they cannot all be supplied.
  virtual bool retransmit(std::uint64_t first, std::uint64_t last, std::vector<FeedInput>& out) = 0;

 protected:
  RecoverySource() = default;
  RecoverySource(const RecoverySource&) = default;
  RecoverySource& operator=(const RecoverySource&) = default;
};

struct FeedConfig {
  itch::Symbol symbol{};  ///< space-padded, e.g. make_symbol("AAPL")
  BookConfig book{};
  std::uint64_t first_seq{1};
};

[[nodiscard]] itch::Symbol make_symbol(std::string_view s) noexcept;

struct FeedCounters {
  std::uint64_t inputs{0};
  std::uint64_t applied{0};       ///< messages applied to our book
  std::uint64_t other_stock{0};
  std::uint64_t skipped{0};       ///< non-subset types
  std::uint64_t duplicates{0};    ///< seq < expected, dropped
  std::uint64_t ignored_not_live{0};
  std::uint64_t gaps{0};
};

class FeedHandler {
 public:
  explicit FeedHandler(const FeedConfig& config, RecoverySource* recovery = nullptr);

  /// Processes one input; appends 0..n events to `out`. On the hot path
  /// (Live, no gap) this does not allocate as long as `out` has capacity.
  void on_input(const FeedInput& in, std::vector<FeedEvent>& out);

  /// Replace the book with `orders` (arrival order) as of sequence `as_of`;
  /// the next expected input is as_of + 1. Allowed in any state. On success
  /// the state becomes Live; if an order is rejected the book is cleared and
  /// the state is Stale.
  void resync_from_snapshot(std::span<const OrderView> orders, std::uint64_t as_of,
                            std::vector<FeedEvent>& out);

  [[nodiscard]] FeedState state() const noexcept { return state_; }
  [[nodiscard]] const Book& book() const noexcept { return book_; }
  [[nodiscard]] std::uint16_t locate() const noexcept { return locate_; }
  [[nodiscard]] std::uint64_t expected_seq() const noexcept { return expected_; }
  [[nodiscard]] const FeedCounters& counters() const noexcept { return counters_; }

 private:
  void process(const FeedInput& in, std::vector<FeedEvent>& out);
  void apply(const FeedInput& in, std::vector<FeedEvent>& out);
  void go(FeedState s, const FeedInput* in, AlertReason why, std::uint8_t detail,
          std::vector<FeedEvent>& out);
  void publish(const FeedInput& in, char type, std::uint64_t ts, std::uint64_t ref, const FeedEvent& what,
               std::vector<FeedEvent>& out);
  void fill_bbo(FeedEvent& e) const noexcept;

  FeedConfig config_;
  RecoverySource* recovery_;
  Book book_;
  FeedState state_{FeedState::Uninitialized};
  std::uint16_t locate_{0};
  bool have_locate_{false};
  std::uint64_t expected_;
  std::uint64_t last_ts_{0};
  FeedCounters counters_{};
  std::vector<FeedInput> retransmit_buf_;
};

}  // namespace lob::md
