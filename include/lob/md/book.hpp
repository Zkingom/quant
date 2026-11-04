#pragma once

// md::Book -- order-book reconstruction for ONE instrument from ITCH order
// messages (L3' 1c). Update semantics only: the book applies what the
// exchange reports (add, execute, cancel, delete, replace); it never matches.
//
// Why not the L1 OrderBook (D-046): L1's add() requires increasing seq and a
// price band, fill_front() can only fill the queue head, and it has no
// partial cancel. ITCH executions name an arbitrary order and partial
// cancels are routine.
//
// Every operation validates BEFORE it mutates; a rejected message leaves the
// book exactly as it was and returns the reason. Checks:
//   - add: ref not live, shares > 0, price > 0, capacity, and the new price
//     must not lock or cross the opposite best (bid < ask strictly)
//   - execute / cancel: ref live, 0 < shares <= remaining
//   - delete: ref live
//   - replace: original live, new ref not live (other than the original),
//     shares > 0, price > 0, no lock/cross at the new price
// find_invariant_violation() re-derives everything (O(n)).
//
// Locked / crossed books (D-054): the "bid < ask" rule is enforced only in
// STRICT mode (the default). The feed handler turns strict mode off outside
// regular trading (pre-market, auctions, halts, pauses, after hours), when a
// real exchange book can legitimately lock or cross; adds and replaces then
// skip the lock/cross check and the invariant check skips "bid < ask".
// cross_state() reports the current condition either way.
//
// Memory: all storage is reserved in the constructor from BookConfig
// (order pool, open-addressing index, per-side level arrays). No member
// function other than the constructor allocates; capacity exhaustion is a
// reported error (OrderCapacity / LevelCapacity), never a reallocation.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lob/md/itch.hpp"

namespace lob::md {

using itch::Side;

struct BookConfig {
  std::size_t max_orders{1U << 20};        ///< live orders
  std::size_t max_levels_per_side{1U << 14};
};

enum class CrossState : std::uint8_t { None, Locked, Crossed };

enum class BookStatus : std::uint8_t {
  Applied,
  DuplicateOrderRef,  ///< add / replace with a ref that is already live
  UnknownOrderRef,    ///< execute / cancel / delete / replace of a ref not live
  ZeroShares,         ///< add / replace / execute / cancel with 0 shares
  ZeroPrice,          ///< add / replace at price 0
  Overfill,           ///< execute or cancel more than the remaining shares
  WouldLock,          ///< new price equals the opposite best
  WouldCross,         ///< new price is through the opposite best
  OrderCapacity,
  LevelCapacity,
};

[[nodiscard]] std::string_view to_string(BookStatus s) noexcept;

struct OrderView {
  std::uint64_t ref;
  Side side;
  std::uint32_t price;
  std::uint32_t shares;  ///< remaining
};

struct LevelView {
  std::uint32_t price;
  std::uint64_t shares;  ///< total remaining at this price
  std::uint32_t orders;
};

/// What the last successful operation changed (for publishing book updates).
struct Change {
  Side side;
  std::uint32_t price;
  std::uint64_t level_shares;  ///< level total after the change (0 = level removed)
  std::uint32_t level_orders;
};

class Book {
 public:
  explicit Book(const BookConfig& config = {});

  Book(const Book&) = delete;
  Book& operator=(const Book&) = delete;
  Book(Book&&) noexcept = default;
  Book& operator=(Book&&) noexcept = default;
  ~Book() = default;

  [[nodiscard]] BookStatus add(std::uint64_t ref, Side side, std::uint32_t shares, std::uint32_t price);
  /// Execution or execution-with-price (the execution price does not change the book).
  [[nodiscard]] BookStatus execute(std::uint64_t ref, std::uint32_t shares);
  [[nodiscard]] BookStatus cancel(std::uint64_t ref, std::uint32_t shares);
  [[nodiscard]] BookStatus remove(std::uint64_t ref);
  /// Replace keeps the side, loses priority (new ref joins the tail of its level).
  [[nodiscard]] BookStatus replace(std::uint64_t old_ref, std::uint64_t new_ref, std::uint32_t shares,
                                   std::uint32_t price);

  /// Clears all orders (used by recovery). Does not allocate.
  void clear() noexcept;

  /// Strict mode enforces best bid < best ask on adds / replaces and in
  /// find_invariant_violation(). Switching it on does not check the book:
  /// the caller decides what a crossed book at that moment means.
  void set_strict(bool strict) noexcept { strict_ = strict; }
  [[nodiscard]] bool strict() const noexcept { return strict_; }
  [[nodiscard]] CrossState cross_state() const noexcept;

  [[nodiscard]] std::optional<OrderView> find(std::uint64_t ref) const noexcept;
  [[nodiscard]] std::optional<LevelView> best(Side side) const noexcept;
  [[nodiscard]] std::optional<LevelView> level(Side side, std::uint32_t price) const noexcept;
  /// Shares resting at `price` on `side` that arrived before order `ref` (0 if
  /// `ref` is not live at that price). O(position in queue).
  [[nodiscard]] std::uint64_t shares_ahead(std::uint64_t ref) const noexcept;
  [[nodiscard]] std::size_t order_count() const noexcept { return live_orders_; }
  [[nodiscard]] std::size_t level_count(Side side) const noexcept;
  [[nodiscard]] const Change& last_change() const noexcept { return last_change_; }

  /// Level `i` from the best (0 = best). Precondition: i < level_count(side).
  [[nodiscard]] LevelView level_at(Side side, std::size_t i) const noexcept;
  /// Orders of the level at `price` in arrival order (f(OrderView)).
  template <typename F>
  void for_each_order_at(Side side, std::uint32_t price, F&& f) const;

  /// FNV-1a 64 over the canonical state: bids best->worst then asks
  /// best->worst; per level its price, total, order count, and then every
  /// order (ref, shares) in arrival order. Independent of storage layout.
  [[nodiscard]] std::uint64_t state_hash() const noexcept;

  /// O(n) recomputation of every structural fact; nullopt when consistent.
  [[nodiscard]] std::optional<std::string> find_invariant_violation() const;

 private:
  static constexpr std::uint32_t kNil = 0xffffffffU;

  struct Node {
    std::uint64_t ref{0};
    std::uint32_t price{0};
    std::uint32_t shares{0};
    std::uint32_t prev{kNil};
    std::uint32_t next{kNil};  // also the free-list link
    Side side{Side::Buy};
  };
  struct Level {
    std::uint32_t price{0};
    std::uint32_t orders{0};
    std::uint64_t shares{0};
    std::uint32_t head{kNil};
    std::uint32_t tail{kNil};
  };
  // Levels of one side sorted so that the BEST price is at the back: bids
  // ascending, asks descending. New prices near the top of book (the common
  // case) are inserted near the back and move few elements.
  using Levels = std::vector<Level>;

  [[nodiscard]] Levels& levels(Side s) noexcept { return s == Side::Buy ? bids_ : asks_; }
  [[nodiscard]] const Levels& levels(Side s) const noexcept { return s == Side::Buy ? bids_ : asks_; }
  /// Index of the first level whose price is not better... see .cpp.
  [[nodiscard]] std::size_t lower_index(Side s, std::uint32_t price) const noexcept;
  [[nodiscard]] Level* find_level(Side s, std::uint32_t price) noexcept;
  [[nodiscard]] const Level* find_level(Side s, std::uint32_t price) const noexcept;
  [[nodiscard]] BookStatus check_new(Side side, std::uint32_t shares, std::uint32_t price,
                                     bool replacing_level_freed) const noexcept;
  void link(std::uint32_t node, Side side, std::uint32_t price) noexcept;
  void unlink(std::uint32_t node) noexcept;
  void reduce(std::uint32_t node, std::uint32_t shares) noexcept;
  void record_change(Side side, std::uint32_t price) noexcept;

  // Open-addressing index ref -> node (linear probing, backward-shift delete).
  [[nodiscard]] std::size_t slot_of(std::uint64_t ref) const noexcept;
  [[nodiscard]] std::uint32_t index_find(std::uint64_t ref) const noexcept;
  void index_insert(std::uint64_t ref, std::uint32_t node) noexcept;
  void index_erase(std::uint64_t ref) noexcept;

  BookConfig config_;
  std::vector<Node> nodes_;
  std::uint32_t free_head_{kNil};
  std::size_t live_orders_{0};
  std::vector<std::uint32_t> index_;  // node + 1; 0 = empty slot
  std::size_t index_mask_{0};
  Levels bids_;
  Levels asks_;
  Change last_change_{};
  bool strict_{true};
};

template <typename F>
void Book::for_each_order_at(Side side, std::uint32_t price, F&& f) const {
  const Level* lvl = find_level(side, price);
  if (lvl == nullptr) {
    return;
  }
  for (std::uint32_t n = lvl->head; n != kNil; n = nodes_[n].next) {
    const Node& node = nodes_[n];
    f(OrderView{node.ref, node.side, node.price, node.shares});
  }
}

}  // namespace lob::md
