#include "lob/strategy/strategy.hpp"

namespace lob::strategy {

namespace {

std::int64_t floor_to(std::int64_t v, std::int64_t tick) noexcept {
  const std::int64_t q = v / tick;
  return (v % tick != 0 && v < 0 ? q - 1 : q) * tick;
}
std::int64_t ceil_to(std::int64_t v, std::int64_t tick) noexcept { return -floor_to(-v, tick); }

}  // namespace

void MarketMaker::cancel_all(Actions& out) {
  for (Quote* q : {&bid_, &ask_}) {
    if (q->id != 0) {
      out.push(OrderRequest{OrderRequest::Kind::Cancel, q->id, q == &bid_ ? Side::Buy : Side::Sell, 0, 0});
      *q = Quote{};
    }
  }
}

void MarketMaker::requote(Side side, std::uint32_t price, bool allowed, Actions& out) {
  Quote& q = side == Side::Buy ? bid_ : ask_;
  if (!allowed || price == 0) {
    if (q.id != 0) {
      out.push(OrderRequest{OrderRequest::Kind::Cancel, q.id, side, 0, 0});
      q = Quote{};
    }
    return;
  }
  if (q.id != 0 && q.price == price) {
    return;
  }
  if (q.id != 0) {
    out.push(OrderRequest{OrderRequest::Kind::Cancel, q.id, side, 0, 0});
  }
  q = Quote{next_id_++, price};
  out.push(OrderRequest{OrderRequest::Kind::New, q.id, side, price, p_.quote_size});
}

void MarketMaker::on_book_update(const BookTop& t, Actions& out) {
  if (!t.live || !t.tradable || t.bid_price == 0 || t.ask_price == 0) {
    cancel_all(out);
    return;
  }
  const auto tick = static_cast<std::int64_t>(p_.tick);
  const std::int64_t bid = t.bid_price;
  const std::int64_t ask = t.ask_price;
  const std::int64_t mid = (bid + ask) / 2;
  const std::int64_t r = mid - p_.skew_per_share * inventory_;
  std::int64_t b = floor_to(r - static_cast<std::int64_t>(p_.half_spread), tick);
  std::int64_t a = ceil_to(r + static_cast<std::int64_t>(p_.half_spread), tick);
  if (b >= ask) b = floor_to(ask - 1, tick);  // stay passive
  if (a <= bid) a = ceil_to(bid + 1, tick);
  const bool buy_ok = inventory_ < p_.max_inventory && b > 0 && b <= INT32_MAX;
  const bool sell_ok = inventory_ > -p_.max_inventory && a > 0 && a <= INT32_MAX;
  requote(Side::Buy, buy_ok ? static_cast<std::uint32_t>(b) : 0U, buy_ok, out);
  requote(Side::Sell, sell_ok ? static_cast<std::uint32_t>(a) : 0U, sell_ok, out);
}

void MarketMaker::on_fill(const Fill& f, Actions&) {
  if (f.qty > 0) {
    inventory_ += f.side == Side::Buy ? static_cast<std::int64_t>(f.qty) : -static_cast<std::int64_t>(f.qty);
  }
  if (f.leaves == 0) {
    if (bid_.id == f.client_id) bid_ = Quote{};
    if (ask_.id == f.client_id) ask_ = Quote{};
  }
}

void MarketMaker::on_timer(std::uint64_t, Actions&) { ++timers_; }

}  // namespace lob::strategy
