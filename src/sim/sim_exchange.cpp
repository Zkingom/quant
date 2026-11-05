#include "lob/sim/sim_exchange.hpp"

#include <algorithm>
#include <variant>

namespace lob::sim {

namespace {

lob::Side to_engine(Side s) noexcept { return s == Side::Buy ? lob::Side::Buy : lob::Side::Sell; }
Side from_engine(lob::Side s) noexcept { return s == lob::Side::Buy ? Side::Buy : Side::Sell; }

/// Is a resting order of ours at `ours` on side `s` at a better price than `px`?
bool better(Side s, std::uint32_t ours, std::uint32_t px) noexcept {
  return s == Side::Sell ? ours < px : ours > px;
}

}  // namespace

SimExchange::SimExchange(const SimConfig& cfg) : cfg_(cfg), engine_(cfg.engine) {
  events_.reserve(256);
}

OpenOrder* SimExchange::find(std::uint64_t id) noexcept {
  for (OpenOrder& o : open_) {
    if (o.id == id) return &o;
  }
  return nullptr;
}

void SimExchange::run_engine(const Command& c, Reports& out) {
  events_.clear();
  engine_.process(c, events_);
  for (const Event& ev : events_) {
    if (const auto* t = std::get_if<TradeExecuted>(&ev)) {
      // Each side of a trade that is one of our orders produces a fill.
      const auto report = [&](OrderId id, lob::Side side, Quantity remaining) {
        if (id >= kSyntheticBase) return;
        OpenOrder* o = find(id);
        if (o == nullptr) return;
        o->leaves = static_cast<std::uint32_t>(remaining);
        out.push(Report{Report::Kind::Fill, id, from_engine(side), static_cast<std::uint32_t>(t->price),
                        static_cast<std::uint32_t>(t->quantity), o->leaves});
        if (o->leaves == 0) *o = OpenOrder{};
      };
      report(t->taker_id, t->taker_side, t->taker_remaining);
      report(t->maker_id, t->taker_side == lob::Side::Buy ? lob::Side::Sell : lob::Side::Buy, t->maker_remaining);
    } else if (const auto* r = std::get_if<OrderRejected>(&ev)) {
      if (r->id < kSyntheticBase && r->command == CommandType::NewOrder) {
        if (OpenOrder* o = find(r->id)) {
          out.push(Report{Report::Kind::Rejected, r->id, o->side, o->price, o->leaves, 0});
          *o = OpenOrder{};
        }
      }
    } else if (const auto* x = std::get_if<OrderCancelled>(&ev)) {
      if (x->id < kSyntheticBase) {
        if (OpenOrder* o = find(x->id)) {
          out.push(Report{Report::Kind::Cancelled, x->id, o->side, o->price, static_cast<std::uint32_t>(x->remaining), 0});
          *o = OpenOrder{};
        }
      }
    }
  }
}

void SimExchange::submit(const OrderRequest& r, const md::Book& book, Reports& out) {
  if (r.kind == OrderRequest::Kind::Cancel) {
    if (find(r.client_id) != nullptr) {
      run_engine(CancelOrder{r.client_id}, out);
    }
    return;
  }
  OpenOrder* slot = find(0);
  if (slot == nullptr || r.client_id == 0 || r.client_id >= kSyntheticBase || find(r.client_id) != nullptr) {
    out.push(Report{Report::Kind::Rejected, r.client_id, r.side, r.price, r.qty, 0});
    return;
  }
  *slot = OpenOrder{r.client_id, r.side, r.price, r.qty, 0, 0};
  if (cfg_.model == FillModel::Conservative) {
    // Back of the displayed queue at our price: everything resting there now
    // is ahead of us. The cutoff is the largest ref among those orders.
    if (const auto lvl = book.level(r.side, r.price)) {
      slot->queue_ahead = lvl->shares;
      std::uint64_t cutoff = 0;
      book.for_each_order_at(r.side, r.price, [&](const md::OrderView& o) { cutoff = std::max(cutoff, o.ref); });
      slot->cutoff_ref = cutoff;
    }
  }
  run_engine(NewOrder{r.client_id, to_engine(r.side), static_cast<Price>(r.price), static_cast<Quantity>(r.qty)}, out);
}

void SimExchange::reduce(std::uint64_t id, std::uint32_t new_leaves, Reports& out) {
  OpenOrder* o = find(id);
  if (o == nullptr || new_leaves == 0 || new_leaves >= o->leaves) return;
  const OpenOrder saved = *o;
  Reports scratch;
  run_engine(CancelOrder{id}, scratch);  // frees the slot
  OpenOrder* slot = find(0);
  *slot = saved;
  slot->leaves = new_leaves;
  out.push(Report{Report::Kind::Cancelled, id, saved.side, saved.price, saved.leaves - new_leaves, new_leaves});
  // Same price as before, so it cannot cross our own orders; any report is forwarded anyway.
  run_engine(NewOrder{id, to_engine(saved.side), static_cast<Price>(saved.price), static_cast<Quantity>(new_leaves)}, out);
}

void SimExchange::sweep(Side aggressor, std::uint32_t limit, std::uint64_t qty, Reports& out) {
  if (qty == 0) return;
  const OrderId id = kSyntheticBase + next_synthetic_++;
  ++synthetic_;
  const auto q = static_cast<Quantity>(std::min<std::uint64_t>(qty, static_cast<std::uint64_t>(cfg_.engine.max_order_quantity)));
  run_engine(NewOrder{id, to_engine(aggressor), static_cast<Price>(limit), q}, out);
  // Immediate-or-cancel: whatever did not trade must not rest.
  if (engine_.book().find(id).has_value()) {
    run_engine(CancelOrder{id}, out);
  }
}

void SimExchange::on_market(const md::FeedEvent& e, Reports& out) {
  if (e.kind != md::FeedEvent::Kind::BookUpdate || e.side == 0) return;
  const Side rest_side = e.side == 'B' ? Side::Buy : Side::Sell;
  const char t = e.msg_type;
  if (t == 'E' || t == 'C') {
    const Side aggressor = rest_side == Side::Buy ? Side::Sell : Side::Buy;
    if (cfg_.model == FillModel::Optimistic) {
      sweep(aggressor, e.price, e.shares, out);
      return;
    }
    // Conservative: the execution's q shares are a budget the aggressor
    // spends on our orders in price priority -- better-priced orders first
    // (trade-through: the aggressor reached a worse price, so it passed
    // ours), then at the execution price after the queue ahead of us.
    std::array<OpenOrder*, kMaxOpen> mine{};
    std::size_t n = 0;
    for (OpenOrder& o : open_) {
      if (o.id != 0 && o.side == rest_side && (o.price == e.price || better(rest_side, o.price, e.price))) {
        mine[n++] = &o;
      }
    }
    std::sort(mine.begin(), mine.begin() + static_cast<std::ptrdiff_t>(n), [&](const OpenOrder* x, const OpenOrder* y) {
      return x->price != y->price ? better(rest_side, x->price, y->price) : x->id < y->id;
    });
    std::uint64_t budget = e.shares;
    std::uint64_t eligible = 0;
    for (std::size_t i = 0; i < n && budget > 0; ++i) {
      OpenOrder& o = *mine[i];
      if (o.price == e.price) {
        const std::uint64_t used = std::min(budget, o.queue_ahead);
        o.queue_ahead -= used;
        budget -= used;
      }
      const std::uint64_t take = std::min<std::uint64_t>(budget, o.leaves);
      budget -= take;
      eligible += take;
    }
    sweep(aggressor, e.price, eligible, out);
    return;
  }
  if (cfg_.model != FillModel::Conservative) return;
  // Orders ahead of us leaving the queue (cancel, delete, replace away).
  auto shrink = [&](std::uint64_t ref, std::uint32_t price, std::uint32_t shares) {
    for (OpenOrder& o : open_) {
      if (o.id != 0 && o.side == rest_side && o.price == price && ref <= o.cutoff_ref) {
        o.queue_ahead -= std::min<std::uint64_t>(o.queue_ahead, shares);
      }
    }
  };
  if (t == 'X' || t == 'D') {
    shrink(e.order_ref, e.price, e.shares);
  } else if (t == 'U' && e.prev_ref != 0) {
    shrink(e.prev_ref, e.prev_price, e.prev_shares);
  }
}

}  // namespace lob::sim
