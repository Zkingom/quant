#include "lob/net/ouch_exchange.hpp"

#include <algorithm>
#include <array>

namespace lob::net {

namespace {

constexpr std::uint64_t kTokenUnit = 1'000'000'000ULL;  // one throttle token in bucket units

std::size_t pow2_at_least(std::size_t n) {
  std::size_t p = 16;
  while (p < n) p <<= 1U;
  return p;
}

bool rejected_in(const sim::Reports& r, std::uint64_t id) {
  for (std::size_t i = 0; i < r.size(); ++i) {
    if (r[i].kind == sim::Report::Kind::Rejected && r[i].client_id == id) return true;
  }
  return false;
}

}  // namespace

OuchExchange::OuchExchange(const ExchangeConfig& cfg)
    : cfg_(cfg), feed_(cfg.feed), sim_(cfg.sim), orders_(), table_(pow2_at_least(cfg.max_orders * 2), 0) {
  mask_ = table_.size() - 1;
  orders_.reserve(cfg.max_orders);
  events_.reserve(64);
  accounts_.resize(cfg.accounts.size());
  for (Account& a : accounts_) {
    a.log = std::make_unique<soup::SessionLog>(cfg.session, cfg.log_messages, cfg.log_bytes);
  }
  // "Start of Day -- always the first message each day" (OUCH 4.2 §3.2).
  for (std::size_t a = 0; a < accounts_.size(); ++a) emit(a, ouch::SystemEvent{0, 'S'});
}

std::size_t OuchExchange::find_account(std::string_view user) const noexcept {
  for (std::size_t a = 0; a < cfg_.accounts.size(); ++a) {
    if (soup::same_field(cfg_.accounts[a].user, user, true)) return a;
  }
  return npos;
}

// --- token table -------------------------------------------------------------------

std::size_t OuchExchange::slot_of(std::size_t a, const ouch::Token& t) const noexcept {
  std::uint64_t h = 0xcbf29ce484222325ULL ^ a;
  for (const char c : t) {
    h ^= static_cast<std::uint8_t>(c);
    h *= 0x100000001b3ULL;
  }
  return static_cast<std::size_t>(h) & mask_;
}

std::uint32_t OuchExchange::find_token(std::size_t a, const ouch::Token& t) const noexcept {
  for (std::size_t s = slot_of(a, t);; s = (s + 1) & mask_) {
    const std::uint32_t v = table_[s];
    if (v == 0) return 0;
    const Order& o = orders_[v - 1];
    if (o.account == a && o.token == t) return v;
  }
}

std::uint32_t OuchExchange::new_order(std::size_t a, const ouch::Token& t) {
  if (orders_.size() >= cfg_.max_orders) {
    ++c_.order_table_full;
    return 0;
  }
  Order o;
  o.account = static_cast<std::uint16_t>(a);
  o.token = t;
  orders_.push_back(o);
  const auto v = static_cast<std::uint32_t>(orders_.size());
  std::size_t s = slot_of(a, t);
  while (table_[s] != 0) s = (s + 1) & mask_;
  table_[s] = v;
  return v;
}

// --- output ------------------------------------------------------------------------

template <class M>
void OuchExchange::emit(std::size_t a, const M& m) {
  std::array<std::uint8_t, ouch::kMaxMessageSize> buf{};
  const std::size_t n = ouch::encode(m, buf);
  if (accounts_[a].log->append({buf.data(), n}) == 0) ++c_.log_refused;
}

void OuchExchange::reject(std::size_t a, const ouch::Token& t, char reason) {
  ++c_.rejected;
  emit(a, ouch::Rejected{market_ts_, t, reason});
}

void OuchExchange::deliver(const sim::Reports& r, char cancel_reason) {
  for (std::size_t i = 0; i < r.size(); ++i) {
    const sim::Report& rep = r[i];
    if (rep.client_id == 0 || rep.client_id > orders_.size()) continue;
    Order& o = orders_[rep.client_id - 1];
    Account& acc = accounts_[o.account];
    switch (rep.kind) {
      case sim::Report::Kind::Fill: {
        ++c_.executions;
        c_.executed_shares += rep.qty;
        o.open = rep.leaves;
        o.executed += rep.qty;
        o.live = rep.leaves > 0;
        const auto q = static_cast<std::int64_t>(rep.qty);
        acc.pos.position += o.side == sim::Side::Buy ? q : -q;
        acc.pos.cash += (o.side == sim::Side::Buy ? -q : q) * static_cast<std::int64_t>(rep.price);
        emit(o.account, ouch::Executed{market_ts_, o.token, rep.qty, rep.price, 'A', next_match_++});
        break;
      }
      case sim::Report::Kind::Cancelled:
        o.open = rep.leaves;
        o.live = rep.leaves > 0;
        emit(o.account, ouch::Canceled{market_ts_, o.token, rep.qty, cancel_reason});
        break;
      case sim::Report::Kind::Rejected:
        o.open = 0;
        o.live = false;
        reject(o.account, o.token, ouch::reason::kRejectOther);
        break;
    }
  }
}

void OuchExchange::cancel_order(std::uint32_t idx, char reason) {
  const Order& o = orders_[idx - 1];
  sim::Reports r;
  sim_.submit(strategy::OrderRequest{strategy::OrderRequest::Kind::Cancel, idx, o.side, 0, 0}, feed_.book(), r);
  deliver(r, reason);
}

// --- market data -------------------------------------------------------------------

std::size_t OuchExchange::apply_input(const md::FeedInput& in) {
  events_.clear();
  feed_.on_input(in, events_);
  if (in.kind == md::InputKind::Decoded) {
    market_ts_ = std::max(market_ts_, md::itch::header_of(in.msg).timestamp);
  }
  return events_.size();
}

void OuchExchange::market_event(std::size_t k) {
  if (k >= events_.size()) return;
  const md::FeedEvent& e = events_[k];
  if (e.kind != md::FeedEvent::Kind::BookUpdate) return;
  sim::Reports r;
  sim_.on_market(e, r);
  deliver(r, ouch::reason::kUserCancel);
}

void OuchExchange::on_market(const md::FeedInput& in) {
  const std::size_t n = apply_input(in);
  for (std::size_t k = 0; k < n; ++k) market_event(k);
}

// --- order entry -------------------------------------------------------------------

bool OuchExchange::throttle(std::size_t a, std::uint64_t now) {
  const AccountConfig& cfg = cfg_.accounts[a];
  if (cfg.throttle_per_sec == 0) return true;
  Account& acc = accounts_[a];
  const std::uint64_t cap = static_cast<std::uint64_t>(std::max<std::uint32_t>(cfg.throttle_burst, 1)) * kTokenUnit;
  if (!acc.primed) {
    acc.bucket = cap;
    acc.last_refill = now;
    acc.primed = true;
  } else if (now > acc.last_refill) {
    // elapsed is capped so that elapsed * rate cannot overflow (the bucket is full by then anyway)
    const std::uint64_t elapsed = std::min<std::uint64_t>(now - acc.last_refill, cap / cfg.throttle_per_sec + 1);
    acc.bucket = std::min(cap, acc.bucket + elapsed * cfg.throttle_per_sec);
    acc.last_refill = now;
  }
  if (acc.bucket < kTokenUnit) return false;
  acc.bucket -= kTokenUnit;
  return true;
}

char OuchExchange::policy(std::uint32_t price, std::uint32_t tif, char display, std::uint32_t min_qty) const {
  if (price == 0 || price > ouch::kMaxPrice) return ouch::reason::kRejectInvalidPrice;  // market / invalid
  if (display != 'A' && display != 'Y') return ouch::reason::kRejectOther;
  if (tif != ouch::kTifImmediateOrCancel && tif != ouch::kTifMarketHours && tif != ouch::kTifSystemHours) {
    return ouch::reason::kRejectOther;
  }
  if (min_qty != 0) return 'N';  // Invalid Minimum Quantity
  return 0;
}

void OuchExchange::on_message(std::size_t a, soup::Bytes payload, std::uint64_t now) {
  ++c_.inbound;
  ouch::Inbound m;
  if (a >= accounts_.size() || ouch::decode_inbound(payload, m) != ouch::DecodeStatus::Decoded) {
    ++c_.invalid_inbound;
    return;
  }
  if (const auto* e = std::get_if<ouch::EnterOrder>(&m)) {
    on_enter(a, *e, now);
  } else if (const auto* r = std::get_if<ouch::ReplaceOrder>(&m)) {
    on_replace(a, *r, now);
  } else {
    on_cancel(a, std::get<ouch::CancelOrder>(m));
  }
}

void OuchExchange::on_enter(std::size_t a, const ouch::EnterOrder& m, std::uint64_t now) {
  ++c_.enters;
  if (find_token(a, m.token) != 0) {
    ++c_.duplicate_tokens;  // "the new order will be ignored" (§2.1)
    return;
  }
  const std::uint32_t idx = new_order(a, m.token);
  if (idx == 0) {
    reject(a, m.token, ouch::reason::kRejectOther);
    return;
  }
  if (!throttle(a, now)) {
    ++c_.throttled;
    reject(a, m.token, ouch::reason::kRejectPortRate);
    return;
  }
  if (m.stock != feed_.symbol()) return reject(a, m.token, ouch::reason::kRejectInvalidStock);
  if (m.side != ouch::Side::Buy && m.side != ouch::Side::Sell) return reject(a, m.token, ouch::reason::kRejectOther);
  if (m.cross_type != 'N') return reject(a, m.token, 'R');  // not allowed in this type of cross
  if (const char why = policy(m.price, m.time_in_force, m.display, m.min_qty); why != 0) {
    return reject(a, m.token, why);
  }
  Order& o = orders_[idx - 1];
  o.side = m.side == ouch::Side::Buy ? sim::Side::Buy : sim::Side::Sell;
  o.price = m.price;
  o.tif = m.time_in_force;
  o.display = m.display;
  sim::Reports r;
  sim_.submit(strategy::OrderRequest{strategy::OrderRequest::Kind::New, idx, o.side, m.price, m.shares}, feed_.book(), r);
  if (rejected_in(r, idx)) {  // e.g. the simulator's open-order table is full
    reject(a, m.token, ouch::reason::kRejectOther);
    return;
  }
  ++c_.accepted;
  o.open = m.shares;
  o.live = true;
  o.ref = next_ref_++;
  ouch::Accepted acc;
  acc.timestamp = market_ts_;
  acc.token = m.token;
  acc.side = m.side;
  acc.shares = m.shares;
  acc.stock = m.stock;
  acc.price = m.price;
  acc.time_in_force = m.time_in_force;
  const bool blank_firm = std::all_of(m.firm.begin(), m.firm.end(), [](char c) { return c == ' '; });
  acc.firm = blank_firm ? ouch::alpha<4>(cfg_.accounts[a].firm) : m.firm;
  acc.display = m.display;
  acc.order_ref = o.ref;
  acc.capacity = m.capacity;
  acc.iso = m.iso;
  acc.min_qty = m.min_qty;
  acc.cross_type = m.cross_type;
  acc.order_state = 'L';
  acc.bbo_weight = ' ';
  emit(a, acc);
  deliver(r, ouch::reason::kUserCancel);  // immediate executions
  if (o.tif == ouch::kTifImmediateOrCancel && orders_[idx - 1].live) cancel_order(idx, ouch::reason::kImmediateOrCancel);
}

void OuchExchange::on_cancel(std::size_t a, const ouch::CancelOrder& m) {
  ++c_.cancels;
  const std::uint32_t idx = find_token(a, m.token);
  if (idx == 0 || !orders_[idx - 1].live || m.shares >= orders_[idx - 1].open) {
    ++c_.ignored_cancels;  // unknown, dead, or superfluous: silently ignored (§2.3)
    return;
  }
  if (m.shares == 0) {
    cancel_order(idx, ouch::reason::kUserCancel);
    return;
  }
  sim::Reports r;
  sim_.reduce(idx, m.shares, r);
  deliver(r, ouch::reason::kUserCancel);
}

void OuchExchange::on_replace(std::size_t a, const ouch::ReplaceOrder& m, std::uint64_t now) {
  ++c_.replaces;
  const std::uint32_t old_idx = find_token(a, m.existing);
  if (old_idx == 0 || !orders_[old_idx - 1].live || find_token(a, m.replacement) != 0) {
    ++c_.ignored_replaces;  // silently ignored (§2.2, case 1)
    return;
  }
  if (orders_.size() >= cfg_.max_orders) {
    ++c_.order_table_full;
    ++c_.ignored_replaces;
    return;
  }
  if (!throttle(a, now)) {  // Rejected: the existing order stays intact, the replacement token is consumed
    ++c_.throttled;
    const std::uint32_t idx = new_order(a, m.replacement);
    if (idx != 0) reject(a, m.replacement, ouch::reason::kRejectPortRate);
    return;
  }
  const Order old = orders_[old_idx - 1];
  if (policy(m.price, m.time_in_force, m.display, m.min_qty) != 0 || m.shares <= old.executed) {
    // Invalid details: the existing order is cancelled; the replacement token is not consumed (§2.2, case 2).
    cancel_order(old_idx, ouch::reason::kUserCancel);
    return;
  }
  // Cancel the existing order silently (Replaced reports it) and enter the replacement.
  {
    sim::Reports gone;
    sim_.submit(strategy::OrderRequest{strategy::OrderRequest::Kind::Cancel, old_idx, old.side, 0, 0}, feed_.book(),
                gone);
    orders_[old_idx - 1].live = false;
    orders_[old_idx - 1].open = 0;
  }
  const std::uint32_t idx = new_order(a, m.replacement);
  Order& o = orders_[idx - 1];
  o.side = old.side;
  o.price = m.price;
  o.tif = m.time_in_force;
  o.display = m.display;
  o.executed = old.executed;
  const std::uint32_t open = m.shares - old.executed;
  sim::Reports r;
  sim_.submit(strategy::OrderRequest{strategy::OrderRequest::Kind::New, idx, o.side, m.price, open}, feed_.book(), r);
  if (rejected_in(r, idx)) {
    emit(a, ouch::Canceled{market_ts_, old.token, old.open, ouch::reason::kUserCancel});
    reject(a, m.replacement, ouch::reason::kRejectOther);
    return;
  }
  o.open = open;
  o.live = true;
  o.ref = next_ref_++;
  ouch::Replaced rep;
  rep.timestamp = market_ts_;
  rep.replacement = m.replacement;
  rep.side = o.side == sim::Side::Buy ? ouch::Side::Buy : ouch::Side::Sell;
  rep.shares = open;
  rep.stock = feed_.symbol();
  rep.price = m.price;
  rep.time_in_force = m.time_in_force;
  rep.firm = ouch::alpha<4>(cfg_.accounts[a].firm);
  rep.display = m.display;
  rep.order_ref = o.ref;
  rep.capacity = 'P';
  rep.iso = m.iso;
  rep.min_qty = m.min_qty;
  rep.cross_type = 'N';
  rep.order_state = 'L';
  rep.previous = old.token;
  rep.bbo_weight = ' ';
  emit(a, rep);
  deliver(r, ouch::reason::kUserCancel);
  if (o.tif == ouch::kTifImmediateOrCancel && orders_[idx - 1].live) cancel_order(idx, ouch::reason::kImmediateOrCancel);
}

void OuchExchange::on_disconnect(std::size_t a) {
  if (a >= accounts_.size() || !cfg_.accounts[a].cancel_on_disconnect) return;
  for (std::uint32_t idx = 1; idx <= orders_.size(); ++idx) {  // ascending order id
    if (orders_[idx - 1].account == a && orders_[idx - 1].live) {
      ++c_.cancelled_on_disconnect;
      cancel_order(idx, ouch::reason::kSystemCancel);
    }
  }
}

void OuchExchange::end_of_day() {
  for (std::size_t a = 0; a < accounts_.size(); ++a) {
    emit(a, ouch::SystemEvent{market_ts_, 'E'});
    accounts_[a].log->end();
  }
}

// --- verification ------------------------------------------------------------------

AccountPosition OuchExchange::position(std::size_t a) const {
  AccountPosition p = accounts_.at(a).pos;
  p.open_buy = p.open_sell = p.live_orders = 0;
  for (const Order& o : orders_) {
    if (o.account == a && o.live) {
      ++p.live_orders;
      (o.side == sim::Side::Buy ? p.open_buy : p.open_sell) += o.open;
    }
  }
  return p;
}

std::uint64_t OuchExchange::outbound_hash(std::size_t a) const {
  std::uint64_t h = 0xcbf29ce484222325ULL;
  const soup::SessionLog& log = *accounts_.at(a).log;
  for (std::uint64_t s = 1; s < log.next_sequence(); ++s) {
    for (const std::uint8_t b : log.at(s)) {
      h ^= b;
      h *= 0x100000001b3ULL;
    }
    h ^= 0xff;  // message boundary
    h *= 0x100000001b3ULL;
  }
  return h;
}

}  // namespace lob::net
