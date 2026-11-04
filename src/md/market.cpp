#include "lob/md/market.hpp"

#include <algorithm>

namespace lob::md {

namespace {
void mix(std::uint64_t& h, std::uint64_t v, int bytes) noexcept {
  for (int i = 0; i < bytes; ++i) {
    h ^= (v >> (8 * i)) & 0xffU;
    h *= 0x100000001b3ULL;
  }
}
}  // namespace

Shard::Shard(const MarketConfig& cfg, std::size_t index, std::size_t shards)
    : cfg_(cfg), index_(index), pool_(std::make_unique<OrderPool>(cfg.max_orders_per_shard)), slots_(kMaxLocates) {
  events_.reserve(64);
  broadcasts_.reserve(kMaxRemembered);
  const std::size_t n = shards == 0 ? 1 : shards;
  for (const InstrumentSpec& spec : cfg_.instruments) {
    if (spec.locate % n != index_ || slots_[spec.locate].h) continue;
    FeedConfig fc;
    fc.symbol = spec.symbol;
    const std::size_t lv = std::max<std::size_t>(spec.levels, 1);
    fc.book = BookConfig{0, lv, lv};  // reserved = capped: no growth, no allocation
    fc.check_sequence = false;
    fc.resume_grace_ns = cfg_.resume_grace_ns;
    fc.resume_grace_messages = cfg_.resume_grace_messages;
    slots_[spec.locate].h = std::make_unique<FeedHandler>(fc, *pool_, spec.locate);
    order_.push_back(spec.locate);
  }
}

void Shard::digest(Slot& s, std::uint16_t locate) {
  (void)locate;
  for (const FeedEvent& e : events_) {
    s.hash = hash_event(e, s.hash);
    ++s.events;
    if (e.kind == FeedEvent::Kind::StateChange && e.state == FeedState::Stale && s.stale_reason == AlertReason::None) {
      s.stale_reason = e.reason;
    }
  }
}

void Shard::apply(const Routed& r) {
  if (r.kind != Routed::Kind::Stock) {
    // Broadcast (System Event) or stream fault: every instrument of the
    // shard, in creation order (deterministic and identical in all modes).
    for (const std::uint16_t loc : order_) {
      Slot& s = slots_[loc];
      events_.clear();
      if (r.kind == Routed::Kind::Fault) {
        s.h->force_stale(r.in, r.fault, events_);
      } else {
        s.h->on_input(r.in, events_);
      }
      digest(s, loc);
    }
    // Remember it: an instrument whose directory arrives later must see the
    // same inputs a standalone handler would have seen. A day has six System
    // Events; faults make every instrument Stale, so a few suffice.
    if (broadcasts_.size() < kMaxRemembered) {
      broadcasts_.push_back(r);
    }
    return;
  }
  const std::uint16_t loc = itch::header_of(r.in.msg).stock_locate;
  Slot& s = slots_[loc];
  if (!s.h) {
    const auto* dir = std::get_if<itch::StockDirectory>(&r.in.msg);
    if (dir == nullptr) {
      ++unknown_locate_;  // message for an instrument whose directory has not arrived
      return;
    }
    FeedConfig fc;
    fc.symbol = dir->stock;
    fc.book = BookConfig{0, cfg_.max_levels_per_side, cfg_.initial_levels_per_side};
    fc.check_sequence = false;
    fc.resume_grace_ns = cfg_.resume_grace_ns;
    fc.resume_grace_messages = cfg_.resume_grace_messages;
    s.h = std::make_unique<FeedHandler>(fc, *pool_, loc);
    order_.push_back(loc);
    ++late_;
    for (const Routed& b : broadcasts_) {  // replay what it missed, in stream order
      events_.clear();
      if (b.kind == Routed::Kind::Fault) {
        s.h->force_stale(b.in, b.fault, events_);
      } else {
        s.h->on_input(b.in, events_);
      }
      digest(s, loc);
    }
  }
  events_.clear();
  s.h->on_input(r.in, events_);
  digest(s, loc);
}

std::vector<StockResult> Shard::results() const {
  std::vector<StockResult> out;
  for (std::size_t loc = 0; loc < slots_.size(); ++loc) {
    const Slot& s = slots_[loc];
    if (!s.h) continue;
    StockResult r;
    r.locate = static_cast<std::uint16_t>(loc);
    const auto& sym = s.h->symbol();
    r.symbol.assign(sym.data(), sym.size());
    while (!r.symbol.empty() && r.symbol.back() == ' ') r.symbol.pop_back();
    r.state = s.h->state();
    r.stale_reason = s.stale_reason;
    r.events = s.events;
    r.event_hash = s.hash;
    r.book_hash = s.h->book().state_hash();
    r.orders = s.h->book().order_count();
    r.peak_levels = std::max(s.h->book().peak_level_count(Side::Buy), s.h->book().peak_level_count(Side::Sell));
    r.counters = s.h->counters();
    out.push_back(std::move(r));
  }
  return out;
}

MarketSummary summarize(const std::vector<std::unique_ptr<Shard>>& shards) {
  MarketSummary m;
  for (const auto& sh : shards) {
    auto r = sh->results();
    m.stocks.insert(m.stocks.end(), r.begin(), r.end());
    m.pool_peak_sum += sh->pool().peak();
    m.unknown_locate += sh->unknown_locate();
    m.late_instruments += sh->late_instruments();
  }
  std::sort(m.stocks.begin(), m.stocks.end(), [](const StockResult& a, const StockResult& b) { return a.locate < b.locate; });
  for (const StockResult& s : m.stocks) {
    std::uint64_t h = kHashSeed;
    mix(h, s.locate, 2);
    mix(h, static_cast<std::uint8_t>(s.state), 1);
    mix(h, s.event_hash, 8);
    mix(h, s.book_hash, 8);
    m.summary_hash += h;  // a sum: independent of the order instruments are visited in
    switch (s.state) {
      case FeedState::Live: ++m.live; break;
      case FeedState::Stale:
        ++m.stale;
        ++m.stale_reasons[std::string(to_string(s.stale_reason))];
        break;
      case FeedState::Uninitialized: ++m.uninitialized; break;
      case FeedState::Recovering: ++m.other; break;
    }
  }
  return m;
}

}  // namespace lob::md
