#include "lob/testkit/describe.hpp"

#include <sstream>
#include <variant>

namespace lob::testkit {

namespace {

struct EventText {
  std::ostringstream& os;

  void operator()(const OrderAccepted& e) const {
    os << '#' << e.seq << " ACCEPTED id=" << e.id << ' ' << to_string(e.side) << ' ' << e.price
       << 'x' << e.quantity;
  }
  void operator()(const OrderRejected& e) const {
    os << '#' << e.seq << " REJECTED " << to_string(e.command) << " id=" << e.id << ' '
       << to_string(e.reason);
  }
  void operator()(const TradeExecuted& e) const {
    os << '#' << e.seq << " TRADE taker=" << e.taker_id << '(' << to_string(e.taker_side)
       << ") maker=" << e.maker_id << ' ' << e.price << 'x' << e.quantity
       << " taker_rem=" << e.taker_remaining << " maker_rem=" << e.maker_remaining;
  }
  void operator()(const OrderRested& e) const {
    os << '#' << e.seq << " RESTED id=" << e.id << ' ' << to_string(e.side) << ' ' << e.price
       << 'x' << e.remaining;
  }
  void operator()(const OrderCancelled& e) const {
    os << '#' << e.seq << " CANCELLED id=" << e.id << ' ' << to_string(e.side) << ' ' << e.price
       << 'x' << e.remaining << ' ' << to_string(e.reason);
  }
};

}  // namespace

std::string describe(const Event& event) {
  std::ostringstream os;
  std::visit(EventText{os}, event);
  return os.str();
}

std::string describe(const std::vector<Event>& events) {
  std::string out;
  for (const Event& e : events) {
    out += describe(e);
    out += '\n';
  }
  return out;
}

std::string describe(const Command& command) {
  std::ostringstream os;
  if (const auto* o = std::get_if<NewOrder>(&command)) {
    os << "NEW id=" << o->id << ' ' << to_string(o->side) << ' ' << o->price << 'x' << o->quantity;
  } else {
    os << "CANCEL id=" << std::get<CancelOrder>(command).id;
  }
  return os.str();
}

std::string dump(const OrderBook& book) {
  std::string out;
  for (const Side side : {Side::Sell, Side::Buy}) {
    out += to_string(side);
    out += ':';
    book.for_each_order(side, [&](Price price, const RestingOrder& o) {
      out += ' ' + std::to_string(price) + '/' + std::to_string(o.id) + 'x' +
             std::to_string(o.remaining) + '#' + std::to_string(o.seq);
    });
    out += '\n';
  }
  return out;
}

}  // namespace lob::testkit
