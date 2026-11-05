#include "lob/gateway/gateway.hpp"

#include <algorithm>
#include <fstream>

namespace lob::gateway {

namespace {

constexpr char kDigits[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";

int base36(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
  return -1;
}

std::size_t pow2_at_least(std::size_t n) {
  std::size_t p = 16;
  while (p < n) p <<= 1U;
  return p;
}

std::size_t mix(std::uint64_t x) noexcept {
  x ^= x >> 33U;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33U;
  return static_cast<std::size_t>(x);
}

ouch::Side to_ouch(sim::Side s) noexcept { return s == sim::Side::Buy ? ouch::Side::Buy : ouch::Side::Sell; }

}  // namespace

std::string_view to_string(OrderState s) noexcept {
  switch (s) {
    case OrderState::PendingNew: return "PendingNew";
    case OrderState::Live: return "Live";
    case OrderState::Filled: return "Filled";
    case OrderState::Canceled: return "Canceled";
    case OrderState::Rejected: return "Rejected";
  }
  return "?";
}

OrderGateway::OrderGateway(const GatewayConfig& cfg) : cfg_(cfg), client_index_(pow2_at_least(cfg.max_orders * 2), 0) {
  client_mask_ = client_index_.size() - 1;
  for (std::size_t i = 0; i < 4; ++i) prefix_[i] = i < cfg.day.size() ? cfg.day[i] : '0';
  prefix_[4] = kDigits[(cfg.instance / 36) % 36];
  prefix_[5] = kDigits[cfg.instance % 36];
  orders_.reserve(cfg.max_orders);
  token_order_.reserve(cfg.max_orders);
  replacement_.reserve(cfg.max_orders);
  replace_args_.reserve(cfg.max_orders);
}

ouch::Token OrderGateway::token_for(std::uint64_t n) const noexcept {
  ouch::Token t{};
  std::copy(prefix_.begin(), prefix_.end(), t.begin());
  for (std::size_t i = 0; i < 8; ++i) {
    t[13 - i] = kDigits[n % 36];
    n /= 36;
  }
  return t;
}

std::int64_t OrderGateway::order_of(const ouch::Token& t) const noexcept {
  if (!std::equal(prefix_.begin(), prefix_.end(), t.begin())) return -1;
  std::uint64_t n = 0;
  for (std::size_t i = 6; i < 14; ++i) {
    const int d = base36(t[i]);
    if (d < 0) return -1;
    n = n * 36 + static_cast<std::uint64_t>(d);
  }
  if (n == 0 || n > token_order_.size()) return -1;
  return token_order_[n - 1];
}

std::int64_t OrderGateway::order_of_client(std::uint64_t client_id) const noexcept {
  for (std::size_t s = mix(client_id) & client_mask_;; s = (s + 1) & client_mask_) {
    const std::uint32_t v = client_index_[s];
    if (v == 0) return -1;
    if (orders_[v - 1].client_id == client_id) return v - 1;
  }
}

std::uint32_t OrderGateway::next_token(std::uint32_t order) noexcept {
  if (token_order_.size() >= cfg_.max_orders) return 0;
  token_order_.push_back(order);
  return static_cast<std::uint32_t>(token_order_.size());
}

const OrderView* OrderGateway::find(std::uint64_t client_id) const noexcept {
  const std::int64_t i = order_of_client(client_id);
  return i < 0 ? nullptr : &orders_[static_cast<std::size_t>(i)];
}

bool OrderGateway::send_enter(OrderView& o, Transport& t) {
  ouch::EnterOrder e;
  e.token = o.token;
  e.side = to_ouch(o.side);
  e.shares = o.qty;
  e.stock = cfg_.symbol;
  e.price = o.price;
  e.time_in_force = cfg_.time_in_force;
  e.firm = ouch::alpha<4>("");
  e.display = cfg_.display;
  std::array<std::uint8_t, ouch::kMaxMessageSize> b{};
  const std::size_t n = ouch::encode(e, b);
  return t.send({b.data(), n});
}

bool OrderGateway::send_cancel(OrderView& o, Transport& t) {
  std::array<std::uint8_t, ouch::kMaxMessageSize> b{};
  std::size_t n = ouch::encode(ouch::CancelOrder{o.token, 0}, b);
  bool ok = t.send({b.data(), n});
  if (o.replace_pending) {
    // The exchange may process the replace first: cancel the replacement
    // as well (a cancel for a token it does not know is ignored).
    const std::size_t idx = static_cast<std::size_t>(&o - orders_.data());
    n = ouch::encode(ouch::CancelOrder{token_for(replacement_[idx]), 0}, b);
    ok = t.send({b.data(), n}) && ok;
  }
  return ok;
}

bool OrderGateway::send_replace(OrderView& o, Transport& t) {
  const std::size_t idx = static_cast<std::size_t>(&o - orders_.data());
  const PendingReplace& a = replace_args_[idx];
  const ouch::ReplaceOrder r{o.token, token_for(replacement_[idx]), a.shares, a.price, cfg_.time_in_force,
                             cfg_.display, 'N', 0};
  std::array<std::uint8_t, ouch::kMaxMessageSize> b{};
  const std::size_t n = ouch::encode(r, b);
  return t.send({b.data(), n});
}

void OrderGateway::submit(const strategy::OrderRequest& r, Transport& t, Reports& out) {
  if (r.kind == strategy::OrderRequest::Kind::Cancel) {
    const std::int64_t i = order_of_client(r.client_id);
    if (i < 0) return;
    OrderView& o = orders_[static_cast<std::size_t>(i)];
    if (final_state(o.state) || o.cancel_requested) return;
    o.cancel_requested = true;
    if (send_cancel(o, t)) {
      ++counters_.sent_cancel;
    } else {
      ++counters_.send_failed;  // resent at the next login
    }
    return;
  }
  const auto local_reject = [&] { out.push(Report{Report::Kind::Rejected, r.client_id, r.side, r.price, r.qty, 0}); };
  if (r.client_id == 0 || order_of_client(r.client_id) >= 0) {  // same rule as SimExchange
    local_reject();
    return;
  }
  if (orders_.size() >= cfg_.max_orders) {
    ++counters_.table_full;
    local_reject();
    return;
  }
  const auto idx = static_cast<std::uint32_t>(orders_.size());
  const std::uint32_t tok = next_token(idx);
  if (tok == 0) {
    ++counters_.table_full;
    local_reject();
    return;
  }
  OrderView o;
  o.client_id = r.client_id;
  o.token = token_for(tok);
  o.side = r.side;
  o.price = r.price;
  o.qty = r.qty;
  o.leaves = r.qty;
  orders_.push_back(o);
  replacement_.push_back(0);
  replace_args_.push_back({});
  std::size_t s = mix(r.client_id) & client_mask_;
  while (client_index_[s] != 0) s = (s + 1) & client_mask_;
  client_index_[s] = idx + 1;
  OrderView& ord = orders_.back();
  if (send_enter(ord, t)) {
    ++counters_.sent_new;
  } else {
    ++counters_.send_failed;
    ord.state = OrderState::Rejected;
    ord.leaves = 0;
    local_reject();
  }
}

bool OrderGateway::replace(std::uint64_t client_id, std::uint32_t shares, std::uint32_t price, Transport& t) {
  const std::int64_t i = order_of_client(client_id);
  if (i < 0) return false;
  const auto idx = static_cast<std::size_t>(i);
  OrderView& o = orders_[idx];
  if (o.state != OrderState::Live || o.replace_pending || o.cancel_requested) return false;
  const std::uint32_t tok = next_token(static_cast<std::uint32_t>(idx));
  if (tok == 0) {
    ++counters_.table_full;
    return false;
  }
  replacement_[idx] = tok;
  replace_args_[idx] = PendingReplace{shares, price};
  o.replace_pending = true;
  if (!send_replace(o, t)) {
    ++counters_.send_failed;  // resent at the next login
    return true;
  }
  ++counters_.sent_replace;
  return true;
}

void OrderGateway::on_message(ouch::Bytes payload, Reports& out) {
  ouch::Outbound m;
  if (ouch::decode_outbound(payload, m) != ouch::DecodeStatus::Decoded) {
    // Includes Skipped types (Broken Trade, ...): the simulator never sends
    // them, and ignoring one could corrupt the position.
    ++counters_.undecodable;
    illegal();
    return;
  }
  if (std::holds_alternative<ouch::SystemEvent>(m)) {
    ++counters_.system_events;
    return;
  }
  const ouch::Token tok = std::visit(
      [](const auto& x) -> ouch::Token {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, ouch::Replaced>) {
          return x.replacement;
        } else if constexpr (std::is_same_v<T, ouch::SystemEvent>) {
          return ouch::Token{};
        } else {
          return x.token;
        }
      },
      m);
  const std::int64_t i = order_of(tok);
  if (i < 0) {
    illegal();  // not one of our tokens
    return;
  }
  const auto idx = static_cast<std::size_t>(i);
  OrderView& o = orders_[idx];
  const bool current = tok == o.token;
  const bool replacement = o.replace_pending && tok == token_for(replacement_[idx]);
  if (final_state(o.state) || (!current && !replacement)) {
    illegal();
    return;
  }

  if (const auto* a = std::get_if<ouch::Accepted>(&m)) {
    if (!current || o.state != OrderState::PendingNew) return illegal();
    ++counters_.accepted;
    o.in_doubt = false;
    o.exchange_ref = a->order_ref;
    o.price = a->price;  // the exchange may re-price (better or equal)
    o.leaves = a->shares;
    if (a->order_state == 'D') {
      o.state = OrderState::Canceled;
      out.push(Report{Report::Kind::Cancelled, o.client_id, o.side, o.price, o.leaves, 0});
      o.leaves = 0;
    } else {
      o.state = OrderState::Live;
    }
  } else if (const auto* j = std::get_if<ouch::Rejected>(&m)) {
    (void)j;
    ++counters_.rejected;
    if (replacement) {  // the replace was refused: the existing order is intact
      o.replace_pending = false;
      replacement_[idx] = 0;
      return;
    }
    if (o.state != OrderState::PendingNew) return illegal();
    o.state = OrderState::Rejected;
    o.in_doubt = false;
    out.push(Report{Report::Kind::Rejected, o.client_id, o.side, o.price, o.qty, 0});
    o.leaves = 0;
  } else if (const auto* e = std::get_if<ouch::Executed>(&m)) {
    if (!current || o.state != OrderState::Live || e->shares == 0 || e->shares > o.leaves) return illegal();
    ++counters_.executions;
    o.leaves -= e->shares;
    o.executed += e->shares;
    out.push(Report{Report::Kind::Fill, o.client_id, o.side, e->price, e->shares, o.leaves});
    if (o.leaves == 0) {
      o.state = OrderState::Filled;
      o.replace_pending = false;
    }
  } else if (const auto* c = std::get_if<ouch::Canceled>(&m)) {
    if (!current || o.state != OrderState::Live || c->decrement == 0 || c->decrement > o.leaves) return illegal();
    ++counters_.canceled;
    o.leaves -= c->decrement;
    out.push(Report{Report::Kind::Cancelled, o.client_id, o.side, o.price, c->decrement, o.leaves});
    if (o.leaves == 0) {
      o.state = OrderState::Canceled;  // includes an invalid replace (§2.2 case 2)
      o.replace_pending = false;
    }
  } else if (const auto* u = std::get_if<ouch::Replaced>(&m)) {
    if (!replacement || u->previous != o.token || o.state != OrderState::Live) return illegal();
    ++counters_.replaced;
    o.token = u->replacement;
    o.price = u->price;
    o.leaves = u->shares;
    o.exchange_ref = u->order_ref;
    o.replace_pending = false;
    replacement_[idx] = 0;
    if (u->order_state == 'D') {
      out.push(Report{Report::Kind::Cancelled, o.client_id, o.side, o.price, o.leaves, 0});
      o.leaves = 0;
      o.state = OrderState::Canceled;
    }
  }
}

void OrderGateway::on_disconnected() {
  for (OrderView& o : orders_) {
    if (!final_state(o.state)) o.in_doubt = true;
  }
}

void OrderGateway::on_logged_in(Transport& t) {
  for (OrderView& o : orders_) {
    if (final_state(o.state)) continue;
    if (o.state == OrderState::PendingNew && o.in_doubt && cfg_.resend_new_on_reconnect) {
      if (send_enter(o, t)) ++counters_.resent;  // same token: ignored if it had arrived
    }
    if (o.cancel_requested) {
      if (send_cancel(o, t)) ++counters_.resent;  // intended size 0: idempotent
    } else if (o.replace_pending) {
      if (send_replace(o, t)) ++counters_.resent;  // a used replacement token is ignored
    }
  }
}

void OrderGateway::on_replay_complete(Reports& out) {
  for (OrderView& o : orders_) {
    if (o.state == OrderState::PendingNew && o.in_doubt && !cfg_.resend_new_on_reconnect) {
      // Everything the exchange had was replayed and this order is not in
      // it: it never arrived. Its token is burnt.
      ++counters_.in_doubt_dropped;
      o.state = OrderState::Rejected;
      out.push(Report{Report::Kind::Rejected, o.client_id, o.side, o.price, o.qty, 0});
      o.leaves = 0;
    }
    o.in_doubt = false;
  }
}

std::size_t OrderGateway::open_ids(std::span<std::uint64_t> out) const {
  std::size_t n = 0;
  for (const OrderView& o : orders_) {
    if (!final_state(o.state) && n < out.size()) out[n++] = o.client_id;
  }
  std::sort(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(n));
  return n;
}

std::uint32_t next_instance(const std::string& path) {
  std::uint32_t v = 0;
  {
    std::ifstream in(path);
    if (in) in >> v;
  }
  std::ofstream out(path, std::ios::trunc);
  out << (v + 1) << '\n';
  return v;
}

}  // namespace lob::gateway
