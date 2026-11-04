#include "lob/md/book.hpp"

#include <algorithm>
#include <bit>
#include <functional>
#include <stdexcept>

namespace lob::md {

namespace {

constexpr std::uint64_t kFnvOffset = 0xcbf29ce484222325ULL;  // same FNV-1a 64 as lob::io
constexpr std::uint64_t kFnvPrime = 0x100000001b3ULL;

void mix(std::uint64_t& h, std::uint64_t v, int bytes) noexcept {
  for (int i = 0; i < bytes; ++i) {
    h ^= (v >> (8 * i)) & 0xffU;
    h *= kFnvPrime;
  }
}

Side opposite(Side s) noexcept { return s == Side::Buy ? Side::Sell : Side::Buy; }

}  // namespace

std::string_view to_string(BookStatus s) noexcept {
  switch (s) {
    case BookStatus::Applied: return "APPLIED";
    case BookStatus::DuplicateOrderRef: return "DUPLICATE_ORDER_REF";
    case BookStatus::UnknownOrderRef: return "UNKNOWN_ORDER_REF";
    case BookStatus::ZeroShares: return "ZERO_SHARES";
    case BookStatus::ZeroPrice: return "ZERO_PRICE";
    case BookStatus::Overfill: return "OVERFILL";
    case BookStatus::WouldLock: return "WOULD_LOCK";
    case BookStatus::WouldCross: return "WOULD_CROSS";
    case BookStatus::OrderCapacity: return "ORDER_CAPACITY";
    case BookStatus::LevelCapacity: return "LEVEL_CAPACITY";
  }
  return "?";
}

OrderPool::OrderPool(std::size_t max_orders) {
  if (max_orders == 0 || max_orders >= kNil / 2) {
    throw std::invalid_argument("md::OrderPool: max_orders out of range");
  }
  nodes.resize(max_orders);
  const std::size_t slots = std::bit_ceil(max_orders * 2);
  index.assign(slots, 0);
  mask = slots - 1;
  reset();
}

void OrderPool::reset() noexcept {
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    nodes[i] = Node{};
    nodes[i].next = (i + 1 < nodes.size()) ? static_cast<std::uint32_t>(i + 1) : kNil;
  }
  free_head = 0;
  live_ = 0;
  std::fill(index.begin(), index.end(), 0U);
}

std::optional<std::string> OrderPool::find_violation() const {
  std::size_t occupied = 0;
  for (const std::uint32_t v : index) {
    occupied += v != 0 ? 1U : 0U;
  }
  if (occupied != live_) {
    return "index occupancy " + std::to_string(occupied) + " != live " + std::to_string(live_);
  }
  std::size_t free_count = 0;
  for (std::uint32_t n = free_head; n != kNil; n = nodes[n].next) {
    if (++free_count > nodes.size()) {
      return "cycle in free list";
    }
  }
  if (free_count + live_ != nodes.size()) {
    return "free list size mismatch";
  }
  return std::nullopt;
}

namespace {
std::size_t reserve_of(const BookConfig& c) {
  return c.initial_levels_per_side == 0 ? c.max_levels_per_side
                                        : std::min(c.initial_levels_per_side, c.max_levels_per_side);
}
}  // namespace

Book::Book(const BookConfig& config) : config_(config) {
  if (config_.max_orders == 0 || config_.max_orders >= kNil / 2 || config_.max_levels_per_side == 0) {
    throw std::invalid_argument("md::BookConfig: max_orders and max_levels_per_side must be in range");
  }
  own_ = std::make_unique<OrderPool>(config_.max_orders);
  pool_ = own_.get();
  bids_.reserve(reserve_of(config_));
  asks_.reserve(reserve_of(config_));
}

Book::Book(const BookConfig& config, OrderPool& shared, std::uint16_t owner)
    : config_(config), pool_(&shared), owner_(owner) {
  if (config_.max_levels_per_side == 0) {
    throw std::invalid_argument("md::BookConfig: max_levels_per_side must be > 0");
  }
  bids_.reserve(reserve_of(config_));
  asks_.reserve(reserve_of(config_));
}

void Book::clear() noexcept {
  if (own_) {
    pool_->reset();
  } else {
    // Shared pool: release only this book's nodes.
    for (Levels* lv : {&bids_, &asks_}) {
      for (const Level& l : *lv) {
        for (std::uint32_t n = l.head; n != kNil;) {
          const std::uint32_t next = pool_->nodes[n].next;
          index_erase(pool_->nodes[n].ref);
          pool_->nodes[n] = Node{};
          pool_->nodes[n].next = pool_->free_head;
          pool_->free_head = n;
          --pool_->live_;
          n = next;
        }
      }
    }
  }
  live_orders_ = 0;
  bids_.clear();
  asks_.clear();
  last_change_ = Change{};
}

std::uint32_t Book::allocate(std::uint64_t ref, Side side, std::uint32_t shares, std::uint32_t price) noexcept {
  // Precondition: free_head != kNil.
  const std::uint32_t n = pool_->free_head;
  pool_->free_head = pool_->nodes[n].next;
  pool_->nodes[n] = Node{ref, price, shares, kNil, kNil, owner_, side};
  index_insert(ref, n);
  ++live_orders_;
  if (++pool_->live_ > pool_->peak_) {
    pool_->peak_ = pool_->live_;
  }
  return n;
}

// ---------------------------------------------------------------- index

std::size_t Book::slot_of(std::uint64_t ref) const noexcept {
  // Fibonacci hashing: order refs are often dense and sequential; the
  // multiply spreads them over the table.
  return static_cast<std::size_t>((ref * 0x9E3779B97F4A7C15ULL) >> 20U) & pool_->mask;
}

std::uint32_t Book::index_find(std::uint64_t ref) const noexcept {
  for (std::size_t i = slot_of(ref);; i = (i + 1) & pool_->mask) {
    const std::uint32_t v = pool_->index[i];
    if (v == 0) {
      return kNil;
    }
    if (pool_->nodes[v - 1].ref == ref) {
      return v - 1;
    }
  }
}

void Book::index_insert(std::uint64_t ref, std::uint32_t node) noexcept {
  std::size_t i = slot_of(ref);
  while (pool_->index[i] != 0) {
    i = (i + 1) & pool_->mask;
  }
  pool_->index[i] = node + 1;
}

void Book::index_erase(std::uint64_t ref) noexcept {
  std::size_t i = slot_of(ref);
  while (pool_->nodes[pool_->index[i] - 1].ref != ref) {  // precondition: present
    i = (i + 1) & pool_->mask;
  }
  pool_->index[i] = 0;
  // Backward-shift deletion: move later members of the probe run into the
  // hole when their home slot is not cyclically in (hole, j].
  std::size_t j = i;
  for (;;) {
    j = (j + 1) & pool_->mask;
    if (pool_->index[j] == 0) {
      return;
    }
    const std::size_t home = slot_of(pool_->nodes[pool_->index[j] - 1].ref);
    const bool home_in_range = (i <= j) ? (home > i && home <= j) : (home > i || home <= j);
    if (!home_in_range) {
      pool_->index[i] = pool_->index[j];
      pool_->index[j] = 0;
      i = j;
    }
  }
}

// ---------------------------------------------------------------- levels

std::size_t Book::lower_index(Side s, std::uint32_t price) const noexcept {
  // Position at which a level with `price` is, or would be inserted, keeping
  // "best at the back": bids ascending, asks descending.
  const Levels& lv = levels(s);
  if (s == Side::Buy) {
    return static_cast<std::size_t>(
        std::lower_bound(lv.begin(), lv.end(), price,
                         [](const Level& l, std::uint32_t p) { return l.price < p; }) -
        lv.begin());
  }
  return static_cast<std::size_t>(
      std::lower_bound(lv.begin(), lv.end(), price,
                       [](const Level& l, std::uint32_t p) { return l.price > p; }) -
      lv.begin());
}

Book::Level* Book::find_level(Side s, std::uint32_t price) noexcept {
  Levels& lv = levels(s);
  const std::size_t i = lower_index(s, price);
  return (i < lv.size() && lv[i].price == price) ? &lv[i] : nullptr;
}

const Book::Level* Book::find_level(Side s, std::uint32_t price) const noexcept {
  const Levels& lv = levels(s);
  const std::size_t i = lower_index(s, price);
  return (i < lv.size() && lv[i].price == price) ? &lv[i] : nullptr;
}

std::size_t Book::level_count(Side side) const noexcept { return levels(side).size(); }

LevelView Book::level_at(Side side, std::size_t i) const noexcept {
  const Levels& lv = levels(side);
  const Level& l = lv[lv.size() - 1 - i];
  return LevelView{l.price, l.shares, l.orders};
}

std::optional<LevelView> Book::best(Side side) const noexcept {
  const Levels& lv = levels(side);
  if (lv.empty()) {
    return std::nullopt;
  }
  return LevelView{lv.back().price, lv.back().shares, lv.back().orders};
}

std::optional<LevelView> Book::level(Side side, std::uint32_t price) const noexcept {
  const Level* l = find_level(side, price);
  if (l == nullptr) {
    return std::nullopt;
  }
  return LevelView{l->price, l->shares, l->orders};
}

std::optional<OrderView> Book::find(std::uint64_t ref) const noexcept {
  const std::uint32_t n = owned_find(ref);
  if (n == kNil) {
    return std::nullopt;
  }
  const Node& node = pool_->nodes[n];
  return OrderView{node.ref, node.side, node.price, node.shares};
}

std::uint64_t Book::shares_ahead(std::uint64_t ref) const noexcept {
  const std::uint32_t n = owned_find(ref);
  if (n == kNil) {
    return 0;
  }
  std::uint64_t ahead = 0;
  for (std::uint32_t p = pool_->nodes[n].prev; p != kNil; p = pool_->nodes[p].prev) {
    ahead += pool_->nodes[p].shares;
  }
  return ahead;
}

void Book::link(std::uint32_t n, Side side, std::uint32_t price) noexcept {
  Levels& lv = levels(side);
  std::size_t i = lower_index(side, price);
  if (i == lv.size() || lv[i].price != price) {
    // Capacity was checked by the caller: this insert never reallocates.
    lv.insert(lv.begin() + static_cast<std::ptrdiff_t>(i), Level{price, 0, 0, kNil, kNil});
    std::size_t& peak = side == Side::Buy ? peak_bids_ : peak_asks_;
    if (lv.size() > peak) peak = lv.size();
  }
  Level& l = lv[i];
  Node& node = pool_->nodes[n];
  node.prev = l.tail;
  node.next = kNil;
  if (l.tail != kNil) {
    pool_->nodes[l.tail].next = n;
  } else {
    l.head = n;
  }
  l.tail = n;
  ++l.orders;
  l.shares += node.shares;
}

void Book::unlink(std::uint32_t n) noexcept {
  Node& node = pool_->nodes[n];
  Levels& lv = levels(node.side);
  const std::size_t i = lower_index(node.side, node.price);
  Level& l = lv[i];  // precondition: present
  if (node.prev != kNil) {
    pool_->nodes[node.prev].next = node.next;
  } else {
    l.head = node.next;
  }
  if (node.next != kNil) {
    pool_->nodes[node.next].prev = node.prev;
  } else {
    l.tail = node.prev;
  }
  --l.orders;
  l.shares -= node.shares;
  if (l.orders == 0) {
    lv.erase(lv.begin() + static_cast<std::ptrdiff_t>(i));
  }
  index_erase(node.ref);
  const Side side = node.side;
  const std::uint32_t price = node.price;
  node = Node{};
  node.next = pool_->free_head;
  pool_->free_head = n;
  --live_orders_;
  --pool_->live_;
  record_change(side, price);
}

void Book::reduce(std::uint32_t n, std::uint32_t shares) noexcept {
  Node& node = pool_->nodes[n];
  if (shares == node.shares) {
    unlink(n);
    return;
  }
  node.shares -= shares;
  find_level(node.side, node.price)->shares -= shares;
  record_change(node.side, node.price);
}

void Book::record_change(Side side, std::uint32_t price) noexcept {
  const Level* l = find_level(side, price);
  last_change_ = Change{side, price, l ? l->shares : 0, l ? l->orders : 0};
}

// ---------------------------------------------------------------- operations

BookStatus Book::check_new(Side side, std::uint32_t shares, std::uint32_t price,
                           bool level_freed) const noexcept {
  if (shares == 0) {
    return BookStatus::ZeroShares;
  }
  if (price == 0) {
    return BookStatus::ZeroPrice;
  }
  const Levels& opp = levels(opposite(side));
  if (strict_ && !opp.empty()) {
    const std::uint32_t o = opp.back().price;
    if (price == o) {
      return BookStatus::WouldLock;
    }
    if (side == Side::Buy ? price > o : price < o) {
      return BookStatus::WouldCross;
    }
  }
  if (find_level(side, price) == nullptr && levels(side).size() >= config_.max_levels_per_side &&
      !level_freed) {
    return BookStatus::LevelCapacity;
  }
  return BookStatus::Applied;
}

BookStatus Book::add(std::uint64_t ref, Side side, std::uint32_t shares, std::uint32_t price) {
  if (index_find(ref) != kNil) {
    return BookStatus::DuplicateOrderRef;
  }
  if (const BookStatus s = check_new(side, shares, price, false); s != BookStatus::Applied) {
    return s;
  }
  if (pool_->free_head == kNil) {
    return BookStatus::OrderCapacity;
  }
  const std::uint32_t n = allocate(ref, side, shares, price);
  link(n, side, price);
  record_change(side, price);
  return BookStatus::Applied;
}

BookStatus Book::execute(std::uint64_t ref, std::uint32_t shares) {
  const std::uint32_t n = owned_find(ref);
  if (n == kNil) {
    return BookStatus::UnknownOrderRef;
  }
  if (shares == 0) {
    return BookStatus::ZeroShares;
  }
  if (shares > pool_->nodes[n].shares) {
    return BookStatus::Overfill;
  }
  reduce(n, shares);
  return BookStatus::Applied;
}

BookStatus Book::cancel(std::uint64_t ref, std::uint32_t shares) { return execute(ref, shares); }

BookStatus Book::remove(std::uint64_t ref) {
  const std::uint32_t n = owned_find(ref);
  if (n == kNil) {
    return BookStatus::UnknownOrderRef;
  }
  unlink(n);
  return BookStatus::Applied;
}

BookStatus Book::replace(std::uint64_t old_ref, std::uint64_t new_ref, std::uint32_t shares,
                         std::uint32_t price) {
  const std::uint32_t old_n = owned_find(old_ref);
  if (old_n == kNil) {
    return BookStatus::UnknownOrderRef;
  }
  if (new_ref != old_ref && index_find(new_ref) != kNil) {
    return BookStatus::DuplicateOrderRef;
  }
  const Node& old = pool_->nodes[old_n];
  const Side side = old.side;
  // The old order's level disappears if it is the only order there and the
  // replacement moves to another price: that frees one level slot.
  const Level* old_level = find_level(side, old.price);
  const bool level_freed = old_level->orders == 1 && old.price != price;
  if (const BookStatus s = check_new(side, shares, price, level_freed); s != BookStatus::Applied) {
    return s;
  }
  unlink(old_n);  // frees a node, so the allocation below cannot fail
  const std::uint32_t n = allocate(new_ref, side, shares, price);
  link(n, side, price);
  record_change(side, price);
  return BookStatus::Applied;
}

// ---------------------------------------------------------------- checks

CrossState Book::cross_state() const noexcept {
  if (bids_.empty() || asks_.empty()) {
    return CrossState::None;
  }
  const std::uint32_t b = bids_.back().price;
  const std::uint32_t a = asks_.back().price;
  return b < a ? CrossState::None : (b == a ? CrossState::Locked : CrossState::Crossed);
}

std::uint64_t Book::state_hash() const noexcept {
  std::uint64_t h = kFnvOffset;
  for (const Side s : {Side::Buy, Side::Sell}) {
    const Levels& lv = levels(s);
    mix(h, lv.size(), 8);
    for (auto it = lv.rbegin(); it != lv.rend(); ++it) {
      mix(h, it->price, 4);
      mix(h, it->shares, 8);
      mix(h, it->orders, 4);
      for (std::uint32_t n = it->head; n != kNil; n = pool_->nodes[n].next) {
        mix(h, pool_->nodes[n].ref, 8);
        mix(h, pool_->nodes[n].shares, 4);
      }
    }
  }
  return h;
}

std::optional<std::string> Book::find_invariant_violation() const {
  std::size_t linked = 0;
  for (const Side s : {Side::Buy, Side::Sell}) {
    const Levels& lv = levels(s);
    const char* name = s == Side::Buy ? "bid" : "ask";
    if (lv.size() > config_.max_levels_per_side) {
      return std::string(name) + " levels exceed capacity";
    }
    for (std::size_t i = 0; i < lv.size(); ++i) {
      const Level& l = lv[i];
      if (i > 0) {
        const bool ordered = s == Side::Buy ? lv[i - 1].price < l.price : lv[i - 1].price > l.price;
        if (!ordered) {
          return std::string(name) + " levels not strictly ordered at " + std::to_string(l.price);
        }
      }
      if (l.orders == 0 || l.shares == 0 || l.price == 0) {
        return std::string(name) + " empty or zero-price level " + std::to_string(l.price);
      }
      std::uint64_t sum = 0;
      std::uint32_t count = 0;
      std::uint32_t prev = kNil;
      for (std::uint32_t n = l.head; n != kNil; n = pool_->nodes[n].next) {
        const Node& node = pool_->nodes[n];
        if (node.prev != prev) {
          return "broken prev link at ref " + std::to_string(node.ref);
        }
        if (node.side != s || node.price != l.price) {
          return "order " + std::to_string(node.ref) + " in the wrong level";
        }
        if (node.shares == 0) {
          return "order " + std::to_string(node.ref) + " has zero shares";
        }
        if (index_find(node.ref) != n) {
          return "index does not point at order " + std::to_string(node.ref);
        }
        sum += node.shares;
        ++count;
        prev = n;
        if (count > live_orders_) {
          return "cycle in level " + std::to_string(l.price);
        }
      }
      if (prev != l.tail) {
        return "level tail mismatch at " + std::to_string(l.price);
      }
      if (sum != l.shares || count != l.orders) {
        return std::string(name) + " level totals mismatch at " + std::to_string(l.price);
      }
      linked += count;
    }
  }
  if (linked != live_orders_) {
    return "linked orders " + std::to_string(linked) + " != live " + std::to_string(live_orders_);
  }
  if (own_) {
    if (auto v = pool_->find_violation()) {  // shared pools are checked by their owner
      return v;
    }
  }
  if (strict_ && cross_state() != CrossState::None) {
    return "book locked or crossed: bid " + std::to_string(bids_.back().price) + " ask " +
           std::to_string(asks_.back().price);
  }
  return std::nullopt;
}

}  // namespace lob::md
