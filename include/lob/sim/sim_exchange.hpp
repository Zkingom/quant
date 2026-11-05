#pragma once

// Simulated exchange for strategy orders (L3' stage 3). DESIGN §7.8.
//
// Orders that pass pre-trade risk go into the EXISTING L1 MatchingEngine
// (unchanged; only its public process() interface is used). That engine
// holds only our orders, so it cannot produce fills on its own; market
// executions reported by ITCH are turned into synthetic aggressive orders:
//
//   ITCH says a resting SELL at price px traded q shares  ==>  a buyer took
//   liquidity at px. We send a synthetic BUY limit px into the engine; it
//   trades against our asks priced at or below px (best first, maker price),
//   and its remainder is cancelled at once (immediate-or-cancel by
//   process(NewOrder) + process(CancelOrder)). Symmetric for resting buys.
//
// Two fill models give an upper and a lower bound of what would really have
// happened (neither models our own market impact):
//   Optimistic   the synthetic order carries all q shares: we are assumed to
//                be at the front of the queue at our price.
//   Conservative our order joined the back of the queue: when it rests we
//                record the displayed shares already at that price
//                (queue_ahead) and the largest order ref there (cutoff;
//                Nasdaq refs increase during the day and a replace gets a
//                new ref, so every order with ref <= cutoff at that price was
//                ahead of us). Executions at our price consume queue_ahead
//                first; cancels / deletes / replaces of orders with
//                ref <= cutoff shrink it. Only the excess reaches us.
// In both models an execution of q shares at a price WORSE than ours (the
// market traded through our price) fills us for up to q shares: the
// aggressor took at least q and would have reached us first. The q shares
// are a budget spent on our orders in price priority. Per order, the
// optimistic cumulative fill is therefore never below the conservative one
// (induction over executions; tested), which is what makes them bounds --
// for the same order flow. A strategy that reacts to its fills sends
// different orders under the two models, so its totals are not ordered.

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "lob/engine/matching_engine.hpp"
#include "lob/md/book.hpp"
#include "lob/md/feed.hpp"
#include "lob/strategy/strategy.hpp"

namespace lob::sim {

using strategy::OrderRequest;
using Side = md::itch::Side;

enum class FillModel : std::uint8_t { Optimistic, Conservative };

/// Execution report back to the OMS / strategy.
struct Report {
  enum class Kind : std::uint8_t { Fill, Rejected, Cancelled };
  Kind kind{Kind::Fill};
  std::uint64_t client_id{0};
  Side side{Side::Buy};
  std::uint32_t price{0};
  std::uint32_t qty{0};     ///< filled (Fill) or removed (Cancelled / Rejected) quantity
  std::uint32_t leaves{0};  ///< open quantity after this report
};

class Reports {
 public:
  static constexpr std::size_t kCapacity = 64;
  void push(const Report& r) noexcept {
    if (n_ < kCapacity) a_[n_++] = r; else ++overflow_;
  }
  void clear() noexcept { n_ = 0; }
  [[nodiscard]] std::size_t size() const noexcept { return n_; }
  [[nodiscard]] const Report& operator[](std::size_t i) const noexcept { return a_[i]; }
  [[nodiscard]] std::uint64_t overflow() const noexcept { return overflow_; }

 private:
  std::array<Report, kCapacity> a_{};
  std::size_t n_{0};
  std::uint64_t overflow_{0};
};

struct SimConfig {
  FillModel model{FillModel::Optimistic};
  EngineConfig engine{1, 0xffffffff, 1'000'000, 4096};
};

struct OpenOrder {
  std::uint64_t id{0};  ///< 0 = free slot
  Side side{Side::Buy};
  std::uint32_t price{0};
  std::uint32_t leaves{0};
  std::uint64_t queue_ahead{0};  ///< conservative model
  std::uint64_t cutoff_ref{0};
};

class SimExchange {
 public:
  static constexpr std::size_t kMaxOpen = 64;

  explicit SimExchange(const SimConfig& cfg);

  /// A risk-approved request. `book` is the market book at this moment
  /// (queue position for the conservative model).
  void submit(const OrderRequest& r, const md::Book& book, Reports& out);
  /// Partial cancel (L4.3, OUCH Cancel with a non-zero intended size): the
  /// order's open quantity becomes `new_leaves` (0 < new_leaves < leaves;
  /// otherwise nothing happens). The L1 engine has no reduce, so the order
  /// is cancelled and re-entered with the same id (dead ids may be reused):
  /// it keeps its place in the PUBLIC queue (the conservative model's
  /// queue_ahead / cutoff are kept) but goes behind our own orders at that
  /// price. Reports one Cancelled with qty = decrement, leaves = new_leaves.
  void reduce(std::uint64_t id, std::uint32_t new_leaves, Reports& out);
  /// A market BookUpdate event (extended FeedEvent fields: side, price,
  /// shares, prev_*). Produces fills of our resting orders.
  void on_market(const md::FeedEvent& e, Reports& out);

  [[nodiscard]] const std::array<OpenOrder, kMaxOpen>& open_orders() const noexcept { return open_; }
  /// Ids of the open orders, ascending (the canonical cancel-all order, D-059).
  std::size_t open_ids(std::span<std::uint64_t> out) const;
  [[nodiscard]] FillModel model() const noexcept { return cfg_.model; }
  [[nodiscard]] const MatchingEngine& engine() const noexcept { return engine_; }
  [[nodiscard]] std::uint64_t synthetic_orders() const noexcept { return synthetic_; }

 private:
  OpenOrder* find(std::uint64_t id) noexcept;
  void run_engine(const Command& c, Reports& out);
  void sweep(Side aggressor, std::uint32_t limit, std::uint64_t qty, Reports& out);

  SimConfig cfg_;
  MatchingEngine engine_;
  std::vector<Event> events_;
  std::array<OpenOrder, kMaxOpen> open_{};
  std::uint64_t next_synthetic_{1};
  std::uint64_t synthetic_{0};
};

/// Synthetic order ids live above every client id.
inline constexpr std::uint64_t kSyntheticBase = 1ULL << 62;

}  // namespace lob::sim
