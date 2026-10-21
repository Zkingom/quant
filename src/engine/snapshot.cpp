#include "lob/engine/snapshot.hpp"

#include <stdexcept>
#include <unordered_set>

#include "lob/core/validation.hpp"

namespace lob {

namespace {

std::string where(std::size_t index) { return "order #" + std::to_string(index) + ": "; }

}  // namespace

std::optional<std::string> find_snapshot_violation(const EngineSnapshot& s) {
  try {
    validate_config(s.config);
  } catch (const std::invalid_argument& e) {
    return std::string("config: ") + e.what();
  }
  if (s.orders.size() > static_cast<std::size_t>(s.config.max_live_orders)) {
    return "more orders than max_live_orders";
  }

  std::unordered_set<OrderId> ids;
  std::unordered_set<SeqNum> seqs;
  ids.reserve(s.orders.size());
  seqs.reserve(s.orders.size());
  const SnapshotOrder* previous = nullptr;
  std::optional<Price> best_bid;
  std::optional<Price> best_ask;

  for (std::size_t i = 0; i < s.orders.size(); ++i) {
    const SnapshotOrder& o = s.orders[i];
    if (const auto reason = validate_new_order(
            NewOrder{.id = o.id, .side = o.side, .price = o.price, .quantity = o.remaining}, s.config)) {
      return where(i) + std::string(to_string(*reason));
    }
    if (o.seq == 0 || o.seq > s.last_seq) {
      return where(i) + "seq " + std::to_string(o.seq) + " outside [1, last_seq " +
             std::to_string(s.last_seq) + "]";
    }
    if (!ids.insert(o.id).second) {
      return where(i) + "duplicate id " + std::to_string(o.id);
    }
    if (!seqs.insert(o.seq).second) {
      return where(i) + "duplicate seq " + std::to_string(o.seq);
    }

    // Canonical order: bids (best first, FIFO), then asks (best first, FIFO).
    if (previous != nullptr) {
      if (previous->side == Side::Sell && o.side == Side::Buy) {
        return where(i) + "bid after asks (not canonical)";
      }
      if (previous->side == o.side) {
        const bool better_or_equal_follows =
            o.side == Side::Buy ? o.price > previous->price : o.price < previous->price;
        if (better_or_equal_follows) {
          return where(i) + "price order not canonical";
        }
        if (o.price == previous->price && o.seq <= previous->seq) {
          return where(i) + "FIFO (seq) order not canonical within price " + std::to_string(o.price);
        }
      }
    }
    if (o.side == Side::Buy && !best_bid) {
      best_bid = o.price;
    }
    if (o.side == Side::Sell && !best_ask) {
      best_ask = o.price;
    }
    previous = &o;
  }
  if (best_bid && best_ask && *best_bid >= *best_ask) {
    return "crossed or locked book: best bid " + std::to_string(*best_bid) + " >= best ask " +
           std::to_string(*best_ask);
  }
  return std::nullopt;
}

}  // namespace lob
