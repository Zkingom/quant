#include "lob/md/feed.hpp"

#include <type_traits>

namespace lob::md {

namespace {

void mix(std::uint64_t& h, std::uint64_t v, int bytes) noexcept {
  for (int i = 0; i < bytes; ++i) {
    h ^= (v >> (8 * i)) & 0xffU;
    h *= 0x100000001b3ULL;
  }
}

std::string_view kind_name(FeedEvent::Kind k) noexcept {
  switch (k) {
    case FeedEvent::Kind::BookUpdate: return "BOOK";
    case FeedEvent::Kind::StateChange: return "STATE";
    case FeedEvent::Kind::Alert: return "ALERT";
    case FeedEvent::Kind::System: return "SYSTEM";
    case FeedEvent::Kind::Trading: return "TRADING";
    case FeedEvent::Kind::Cross: return "CROSS";
  }
  return "?";
}

template <class... Ts>
struct Overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

}  // namespace

std::string_view to_string(FeedState s) noexcept {
  switch (s) {
    case FeedState::Uninitialized: return "UNINITIALIZED";
    case FeedState::Live: return "LIVE";
    case FeedState::Recovering: return "RECOVERING";
    case FeedState::Stale: return "STALE";
  }
  return "?";
}

std::string_view to_string(AlertReason r) noexcept {
  switch (r) {
    case AlertReason::None: return "-";
    case AlertReason::SequenceGap: return "SEQUENCE_GAP";
    case AlertReason::GapRecovered: return "GAP_RECOVERED";
    case AlertReason::RecoveryFailed: return "RECOVERY_FAILED";
    case AlertReason::TimestampRegression: return "TIMESTAMP_REGRESSION";
    case AlertReason::BookRejected: return "BOOK_REJECTED";
    case AlertReason::SymbolMismatch: return "SYMBOL_MISMATCH";
    case AlertReason::DecodeError: return "DECODE_ERROR";
    case AlertReason::FrameError: return "FRAME_ERROR";
    case AlertReason::SnapshotLoaded: return "SNAPSHOT_LOADED";
    case AlertReason::SnapshotRejected: return "SNAPSHOT_REJECTED";
    case AlertReason::CrossedAtResume: return "CROSSED_AT_RESUME";
    case AlertReason::ResumeGrace: return "RESUME_GRACE";
    case AlertReason::GraceResolved: return "GRACE_RESOLVED";
  }
  return "?";
}

itch::Symbol make_symbol(std::string_view s) noexcept {
  itch::Symbol out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = i < s.size() ? s[i] : ' ';
  }
  return out;
}

void append_text(const FeedEvent& e, std::string& out) {
  out += "seq=";
  out += std::to_string(e.seq);
  out += " ts=";
  out += std::to_string(e.timestamp);
  out += ' ';
  out += kind_name(e.kind);
  out += " state=";
  out += to_string(e.state);
  out += " reason=";
  out += to_string(e.reason);
  out += " detail=";
  out += std::to_string(e.detail);
  out += " type=";
  out += e.msg_type == 0 ? '-' : e.msg_type;
  out += " ref=";
  out += std::to_string(e.order_ref);
  out += " bid=";
  out += std::to_string(e.bid_price);
  out += 'x';
  out += std::to_string(e.bid_shares);
  out += " ask=";
  out += std::to_string(e.ask_price);
  out += 'x';
  out += std::to_string(e.ask_shares);
  if (e.kind == FeedEvent::Kind::BookUpdate) {
    out += " msg=";
    out += e.side == 0 ? '-' : e.side;
    out += std::to_string(e.price);
    out += 'x';
    out += std::to_string(e.shares);
    if (e.prev_ref != 0) {
      out += " prev=";
      out += std::to_string(e.prev_ref);
      out += '@';
      out += std::to_string(e.prev_price);
      out += 'x';
      out += std::to_string(e.prev_shares);
    }
  }
  out += " sess=";
  out += e.session == 0 ? '-' : e.session;
  out += " trd=";
  out += e.trading == 0 ? '-' : e.trading;
  out += " cross=";
  out += e.cross;
  if (e.grace != '-') {
    out += " grace=";
    out += e.grace;
  }
  if (e.reason == AlertReason::SequenceGap) {
    out += " gap=";
    out += std::to_string(e.gap_first);
    out += '-';
    out += std::to_string(e.gap_last);
  }
  out += '\n';
}

std::uint64_t hash_event(const FeedEvent& e, std::uint64_t h) noexcept {
  mix(h, static_cast<std::uint8_t>(e.kind), 1);
  mix(h, static_cast<std::uint8_t>(e.state), 1);
  mix(h, static_cast<std::uint8_t>(e.reason), 1);
  mix(h, e.detail, 1);
  mix(h, static_cast<std::uint8_t>(e.msg_type), 1);
  mix(h, e.seq, 8);
  mix(h, e.timestamp, 8);
  mix(h, e.order_ref, 8);
  mix(h, e.bid_price, 4);
  mix(h, e.ask_price, 4);
  mix(h, e.bid_shares, 8);
  mix(h, e.ask_shares, 8);
  mix(h, e.gap_first, 8);
  mix(h, e.gap_last, 8);
  mix(h, static_cast<std::uint8_t>(e.side), 1);
  mix(h, e.price, 4);
  mix(h, e.shares, 4);
  mix(h, e.prev_ref, 8);
  mix(h, e.prev_price, 4);
  mix(h, e.prev_shares, 4);
  mix(h, static_cast<std::uint8_t>(e.session), 1);
  mix(h, static_cast<std::uint8_t>(e.trading), 1);
  mix(h, static_cast<std::uint8_t>(e.cross), 1);
  if (e.grace != '-') {  // optional field, like its text
    mix(h, static_cast<std::uint8_t>(e.grace), 1);
  }
  return h;
}

FeedHandler::FeedHandler(const FeedConfig& config, RecoverySource* recovery)
    : config_(config), recovery_(recovery), book_(config.book), expected_(config.first_seq) {
  book_.set_strict(false);  // no phase known yet: not regular trading
}

void FeedHandler::fill_bbo(FeedEvent& e) const noexcept {
  e.session = session_;
  e.trading = trading_;
  e.grace = in_grace_ ? 'G' : '-';
  switch (book_.cross_state()) {
    case CrossState::None: e.cross = '-'; break;
    case CrossState::Locked: e.cross = 'L'; break;
    case CrossState::Crossed: e.cross = 'X'; break;
  }
  if (const auto b = book_.best(Side::Buy)) {
    e.bid_price = b->price;
    e.bid_shares = b->shares;
  }
  if (const auto a = book_.best(Side::Sell)) {
    e.ask_price = a->price;
    e.ask_shares = a->shares;
  }
}

void FeedHandler::go(FeedState s, const FeedInput* in, AlertReason why, std::uint8_t detail,
                     std::vector<FeedEvent>& out) {
  if (s == FeedState::Stale) {
    in_grace_ = false;  // a Stale book has no grace left to run
  }
  FeedEvent e{};
  e.kind = (s != state_) ? FeedEvent::Kind::StateChange : FeedEvent::Kind::Alert;
  state_ = s;
  e.state = s;
  e.reason = why;
  e.detail = detail;
  if (in != nullptr) {
    e.seq = in->seq;
    if (in->kind == InputKind::Decoded) {
      const auto& h = itch::header_of(in->msg);
      e.timestamp = h.timestamp;
      e.msg_type = h.type;
    }
  }
  fill_bbo(e);
  out.push_back(e);
}

void FeedHandler::publish(const FeedInput& in, char type, std::uint64_t ts, std::uint64_t ref,
                          const FeedEvent& what, std::vector<FeedEvent>& out) {
  FeedEvent e = what;
  e.kind = FeedEvent::Kind::BookUpdate;
  e.state = state_;
  e.msg_type = type;
  e.seq = in.seq;
  e.timestamp = ts;
  e.order_ref = ref;
  fill_bbo(e);
  out.push_back(e);
}

void FeedHandler::on_input(const FeedInput& in, std::vector<FeedEvent>& out) {
  ++counters_.inputs;
  if (in.seq < expected_) {
    ++counters_.duplicates;
    return;
  }
  if (in.seq > expected_) {
    ++counters_.gaps;
    const std::uint64_t first = expected_;
    const std::uint64_t last = in.seq - 1;
    if (state_ == FeedState::Stale) {
      expected_ = in.seq;  // already unusable; keep counting
    } else {
      const FeedState resume = have_locate_ ? FeedState::Live : FeedState::Uninitialized;
      go(FeedState::Recovering, &in, AlertReason::SequenceGap, 0, out);
      out.back().gap_first = first;
      out.back().gap_last = last;
      retransmit_buf_.clear();
      bool filled = recovery_ != nullptr && recovery_->retransmit(first, last, retransmit_buf_) &&
                    retransmit_buf_.size() == last - first + 1;
      if (filled) {
        for (std::size_t i = 0; i < retransmit_buf_.size(); ++i) {
          if (retransmit_buf_[i].seq != first + i) {
            filled = false;
            break;
          }
        }
      }
      if (filled) {
        for (const FeedInput& r : retransmit_buf_) {
          process(r, out);
          if (state_ == FeedState::Stale) {
            break;
          }
        }
        if (state_ == FeedState::Recovering) {
          go(resume, &in, AlertReason::GapRecovered, 0, out);
        }
      } else {
        go(FeedState::Stale, &in, AlertReason::RecoveryFailed, 0, out);
        expected_ = in.seq;
      }
    }
  }
  process(in, out);
}

void FeedHandler::process(const FeedInput& in, std::vector<FeedEvent>& out) {
  expected_ = in.seq + 1;
  switch (in.kind) {
    case InputKind::FrameError:
      if (state_ != FeedState::Stale) go(FeedState::Stale, &in, AlertReason::FrameError, 0, out);
      return;
    case InputKind::DecodeError:
      if (state_ != FeedState::Stale) go(FeedState::Stale, &in, AlertReason::DecodeError, in.error, out);
      return;
    case InputKind::Skipped:
      ++counters_.skipped;
      return;
    case InputKind::Decoded:
      break;
  }
  const itch::Header& h = itch::header_of(in.msg);
  if (h.timestamp < last_ts_) {
    if (state_ != FeedState::Stale) go(FeedState::Stale, &in, AlertReason::TimestampRegression, 0, out);
    return;
  }
  last_ts_ = h.timestamp;
  if (in_grace_) [[unlikely]] {
    check_grace_timeout(in, out);  // cold, out of line: keeps this hot path a leaf with a tail call
  }
  if (state_ == FeedState::Stale) {
    // The book is unusable, but the trading phase is still followed so that
    // a later snapshot resync knows which rules apply.
    if (!track_phase(in)) {
      ++counters_.ignored_not_live;
    } else {
      book_.set_strict(session_ == 'Q' && trading_ == 'T');
    }
    return;
  }
  apply(in, out);
}

bool FeedHandler::track_phase(const FeedInput& in) {
  if (const auto* se = std::get_if<itch::SystemEvent>(&in.msg)) {
    session_ = se->event_code;
    ++counters_.session_events;
    return true;
  }
  if (const auto* ta = std::get_if<itch::TradingAction>(&in.msg)) {
    if (ta->stock == config_.symbol) {
      trading_ = ta->trading_state;
      ++counters_.trading_actions;
      return true;
    }
  }
  if (const auto* ct = std::get_if<itch::CrossTrade>(&in.msg)) {
    if (ct->stock == config_.symbol) {
      cross_seen_ = true;
      ++counters_.cross_trades;
      return true;
    }
  }
  return false;
}

void FeedHandler::update_strictness(const FeedInput& in, std::vector<FeedEvent>& out) {
  const bool wanted = session_ == 'Q' && trading_ == 'T';
  if (!wanted) {
    if (book_.strict() || in_grace_) {
      book_.set_strict(false);
      in_grace_ = false;     // a new halt / pause / close cancels the grace
      cross_seen_ = false;   // the next resumption needs its own cross
    }
    return;
  }
  if (book_.strict()) {
    return;
  }
  if (in_grace_) {
    try_resolve_grace(in, out);
    return;
  }
  const CrossState cs = book_.cross_state();
  if (cs == CrossState::None) {
    book_.set_strict(true);
    return;
  }
  const auto detail = static_cast<std::uint8_t>(cs == CrossState::Locked ? 'L' : 'X');
  if (grace_enabled()) {
    // The cross's executions may still be on their way (D-055).
    in_grace_ = true;
    grace_start_ts_ = itch::header_of(in.msg).timestamp;
    grace_msgs_ = 0;
    ++counters_.graces;
    go(state_, &in, AlertReason::ResumeGrace, detail, out);
    return;
  }
  // Regular trading cannot begin on a locked or crossed book: the auction or
  // halt cross should have resolved it. The book (or the feed) is wrong.
  book_.set_strict(true);
  go(FeedState::Stale, &in, AlertReason::CrossedAtResume, detail, out);
}

void FeedHandler::try_resolve_grace(const FeedInput& in, std::vector<FeedEvent>& out) {
  if (!in_grace_ || !cross_seen_ || book_.cross_state() != CrossState::None) {
    return;
  }
  in_grace_ = false;
  book_.set_strict(true);
  ++counters_.graces_resolved;
  go(state_, &in, AlertReason::GraceResolved, 0, out);
}

void FeedHandler::check_grace_timeout(const FeedInput& in, std::vector<FeedEvent>& out) {
  if (state_ != FeedState::Stale && itch::header_of(in.msg).timestamp - grace_start_ts_ > config_.resume_grace_ns) {
    expire_grace(in, out);  // the book did not clear in time
  }
}

void FeedHandler::expire_grace(const FeedInput& in, std::vector<FeedEvent>& out) {
  const CrossState cs = book_.cross_state();
  in_grace_ = false;
  book_.set_strict(true);
  go(FeedState::Stale, &in, AlertReason::CrossedAtResume,
     static_cast<std::uint8_t>(cs == CrossState::Locked ? 'L' : cs == CrossState::Crossed ? 'X' : '-'), out);
}

void FeedHandler::apply(const FeedInput& in, std::vector<FeedEvent>& out) {
  const itch::Header& h = itch::header_of(in.msg);
  if (track_phase(in)) {
    FeedEvent e{};
    if (const auto* se = std::get_if<itch::SystemEvent>(&in.msg)) {
      e.kind = FeedEvent::Kind::System;
      e.detail = static_cast<std::uint8_t>(se->event_code);
    } else if (const auto* ta = std::get_if<itch::TradingAction>(&in.msg)) {
      e.kind = FeedEvent::Kind::Trading;
      e.detail = static_cast<std::uint8_t>(ta->trading_state);
    } else {
      e.kind = FeedEvent::Kind::Cross;
      e.detail = static_cast<std::uint8_t>(std::get<itch::CrossTrade>(in.msg).cross_type);
    }
    e.state = state_;
    e.msg_type = h.type;
    e.seq = in.seq;
    e.timestamp = h.timestamp;
    fill_bbo(e);
    out.push_back(e);
    update_strictness(in, out);
    return;
  }
  if (std::holds_alternative<itch::TradingAction>(in.msg) || std::holds_alternative<itch::CrossTrade>(in.msg)) {
    ++counters_.other_stock;  // another instrument's trading state or cross
    return;
  }
  if (const auto* dir = std::get_if<itch::StockDirectory>(&in.msg)) {
    if (dir->stock == config_.symbol && !have_locate_) {
      have_locate_ = true;
      locate_ = h.stock_locate;
      if (state_ == FeedState::Uninitialized) {
        go(FeedState::Live, &in, AlertReason::None, 0, out);
      }
    }
    return;
  }
  if (!have_locate_ || h.stock_locate != locate_) {
    ++counters_.other_stock;
    return;
  }

  BookStatus st = BookStatus::Applied;
  std::uint64_t ref = 0;
  bool symbol_mismatch = false;
  FeedEvent what{};
  auto describe = [&](std::uint64_t r, std::uint32_t qty) {
    if (const auto o = book_.find(r)) {
      what.side = static_cast<char>(o->side);
      what.price = o->price;
      what.shares = qty == 0 ? o->shares : qty;
    }
  };
  std::visit(Overloaded{
                 [&](const itch::AddOrder& m) {
                   ref = m.order_ref;
                   if (m.stock != config_.symbol) {
                     symbol_mismatch = true;
                     return;
                   }
                   what.side = static_cast<char>(m.side);
                   what.price = m.price;
                   what.shares = m.shares;
                   st = book_.add(m.order_ref, m.side, m.shares, m.price);
                 },
                 [&](const itch::OrderExecuted& m) {
                   ref = m.order_ref;
                   describe(m.order_ref, m.executed_shares);
                   st = book_.execute(m.order_ref, m.executed_shares);
                 },
                 [&](const itch::OrderExecutedWithPrice& m) {
                   ref = m.order_ref;
                   describe(m.order_ref, m.executed_shares);
                   st = book_.execute(m.order_ref, m.executed_shares);
                 },
                 [&](const itch::OrderCancel& m) {
                   ref = m.order_ref;
                   describe(m.order_ref, m.cancelled_shares);
                   st = book_.cancel(m.order_ref, m.cancelled_shares);
                 },
                 [&](const itch::OrderDelete& m) {
                   ref = m.order_ref;
                   describe(m.order_ref, 0);
                   st = book_.remove(m.order_ref);
                 },
                 [&](const itch::OrderReplace& m) {
                   ref = m.new_order_ref;
                   if (const auto o = book_.find(m.original_order_ref)) {
                     what.side = static_cast<char>(o->side);
                     what.prev_ref = o->ref;
                     what.prev_price = o->price;
                     what.prev_shares = o->shares;
                   }
                   what.price = m.price;
                   what.shares = m.shares;
                   st = book_.replace(m.original_order_ref, m.new_order_ref, m.shares, m.price);
                 },
                 [](const itch::SystemEvent&) {},
                 [](const itch::StockDirectory&) {},
                 [](const itch::TradingAction&) {},
                 [](const itch::CrossTrade&) {},
             },
             in.msg);
  if (symbol_mismatch) {
    go(FeedState::Stale, &in, AlertReason::SymbolMismatch, 0, out);
    return;
  }
  if (st != BookStatus::Applied) {
    go(FeedState::Stale, &in, AlertReason::BookRejected, static_cast<std::uint8_t>(st), out);
    return;
  }
  ++counters_.applied;
  switch (book_.cross_state()) {  // only possible outside strict mode
    case CrossState::None: break;
    case CrossState::Locked: ++counters_.locked_updates; break;
    case CrossState::Crossed: ++counters_.crossed_updates; break;
  }
  if (state_ == FeedState::Live) {
    publish(in, h.type, h.timestamp, ref, what, out);
  }
  if (in_grace_) {
    ++grace_msgs_;
    try_resolve_grace(in, out);
    if (in_grace_ && grace_msgs_ > config_.resume_grace_messages) {
      expire_grace(in, out);
    }
  }
}

void FeedHandler::resync_from_snapshot(std::span<const OrderView> orders, std::uint64_t as_of,
                                       std::vector<FeedEvent>& out) {
  book_.clear();
  expected_ = as_of + 1;
  FeedInput marker{};
  marker.seq = as_of;
  marker.kind = InputKind::Skipped;
  if (!have_locate_) {
    go(FeedState::Stale, &marker, AlertReason::SnapshotRejected, 0, out);
    return;
  }
  for (const OrderView& o : orders) {
    const BookStatus st = book_.add(o.ref, o.side, o.shares, o.price);
    if (st != BookStatus::Applied) {
      book_.clear();
      go(FeedState::Stale, &marker, AlertReason::SnapshotRejected, static_cast<std::uint8_t>(st), out);
      return;
    }
  }
  go(FeedState::Live, &marker, AlertReason::SnapshotLoaded, 0, out);
}

}  // namespace lob::md
