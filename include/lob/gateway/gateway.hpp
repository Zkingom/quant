#pragma once

// Client order gateway (L4.4): sits after pre-trade risk, turns order
// requests into OUCH 4.2 inbound messages and OUCH outbound messages back
// into the execution reports the OMS / strategy already understand
// (sim::Report -- the same reports the in-process SimExchange produces).
// sans-IO: the transport is an interface; time is not needed.
// L4 design §2.3.
//
// Three numbers per order (§2.3): client_id (strategy), the OUCH token
// (this gateway; a replace gets a new one), the exchange's Order
// Reference Number (from Accepted / Replaced, recorded only).
//
// Tokens: [day 4][instance 2, base 36][counter 8, base 36] = 14 characters.
// `instance` must differ between process starts of the same day (the
// caller persists it, see next_instance()); the counter starts at 1. Our
// own tokens therefore map back to their record by the counter alone --
// no hash table, no allocation; a token that is not ours is "unknown".
//
// Per-order state machine (each transition has a named test):
//   PendingNew --Accepted L--> Live --Executed (all)--> Filled
//   PendingNew --Accepted D--> Canceled          Live --Executed (part)--> Live
//   PendingNew --Rejected----> Rejected          Live --Canceled (to 0)--> Canceled
//   Live / PendingNew --cancel()--> cancel flagged (X sent; Executed may still arrive)
//   Live --replace()--> replace flagged (U sent) --Replaced--> Live (new token)
//                                               --Canceled (old token)--> Canceled
//                                               --Rejected (new token)--> Live, unchanged
// Anything else -- unknown token, an execution larger than the open
// quantity, a message for an order in a final state -- is an ILLEGAL
// transition: counted, and inconsistent() becomes true (the caller trips
// the kill switch: the gateway's view of the position can no longer be
// trusted, L4 design §2.3).
//
// Reconnect (§2.3): on_disconnected() marks every non-final order in doubt;
// on_logged_in() resends the cancels and replaces still pending (benign:
// cancels carry the intended size, used replacement tokens are ignored),
// and new orders only if resend_new_on_reconnect; on_replay_complete()
// (the first Server Heartbeat after the login: the server has nothing left
// to send) turns new orders still unanswered into Rejected -- they never
// reached the exchange.

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "lob/ouch/ouch.hpp"
#include "lob/sim/sim_exchange.hpp"
#include "lob/strategy/strategy.hpp"

namespace lob::gateway {

using sim::Report;
using sim::Reports;

enum class OrderState : std::uint8_t { PendingNew, Live, Filled, Canceled, Rejected };

[[nodiscard]] std::string_view to_string(OrderState s) noexcept;
[[nodiscard]] constexpr bool final_state(OrderState s) noexcept {
  return s == OrderState::Filled || s == OrderState::Canceled || s == OrderState::Rejected;
}

/// Where encoded OUCH inbound messages go (soup::ClientSession::send in
/// practice). False = not sent (not logged in, buffer full).
class Transport {
 public:
  virtual ~Transport() = default;
  virtual bool send(ouch::Bytes payload) = 0;
};

struct GatewayConfig {
  ouch::Symbol symbol{ouch::make_symbol("ACME")};
  std::string day{"0000"};      ///< 4 characters of the token prefix (e.g. MMDD)
  std::uint32_t instance{0};    ///< process start number of the day, < 36^2
  std::size_t max_orders{1U << 16};  ///< tokens per day (new orders + replacements)
  std::uint32_t time_in_force{ouch::kTifSystemHours};
  char display{'A'};
  bool resend_new_on_reconnect{false};
};

struct GatewayCounters {
  std::uint64_t sent_new{0};
  std::uint64_t sent_cancel{0};
  std::uint64_t sent_replace{0};
  std::uint64_t send_failed{0};  ///< transport refused: rejected locally
  std::uint64_t table_full{0};
  std::uint64_t accepted{0};
  std::uint64_t rejected{0};
  std::uint64_t executions{0};
  std::uint64_t canceled{0};
  std::uint64_t replaced{0};
  std::uint64_t system_events{0};
  std::uint64_t resent{0};
  std::uint64_t in_doubt_dropped{0};  ///< new orders that never reached the exchange
  std::uint64_t illegal{0};
  std::uint64_t undecodable{0};
};

struct OrderView {
  std::uint64_t client_id{0};
  ouch::Token token{};
  sim::Side side{sim::Side::Buy};
  std::uint32_t price{0};
  std::uint32_t qty{0};       ///< entered
  std::uint32_t leaves{0};    ///< open on the exchange (as far as we know)
  std::uint32_t executed{0};  ///< cumulative over the replace chain
  std::uint64_t exchange_ref{0};
  OrderState state{OrderState::PendingNew};
  bool cancel_requested{false};
  bool replace_pending{false};
  bool in_doubt{false};
};

class OrderGateway {
 public:
  explicit OrderGateway(const GatewayConfig& cfg);

  /// A risk-approved request. New: Enter Order (a local Rejected report if
  /// it cannot be sent). Cancel: Cancel Order with intended size 0 for a
  /// non-final order (unknown / final: nothing, like SimExchange).
  void submit(const strategy::OrderRequest& r, Transport& t, Reports& out);
  /// Replace Order (same side; new total liable shares and price). False if
  /// the order is not Live, a replace is already pending, or it cannot be sent.
  bool replace(std::uint64_t client_id, std::uint32_t shares, std::uint32_t price, Transport& t);

  /// One OUCH outbound payload (a Sequenced Data message).
  void on_message(ouch::Bytes payload, Reports& out);

  void on_disconnected();
  void on_logged_in(Transport& t);
  void on_replay_complete(Reports& out);

  /// Client ids of the non-final orders, ascending (cancel-all order, D-0xx).
  std::size_t open_ids(std::span<std::uint64_t> out) const;
  [[nodiscard]] const OrderView* find(std::uint64_t client_id) const noexcept;
  [[nodiscard]] bool inconsistent() const noexcept { return counters_.illegal > 0; }
  [[nodiscard]] const GatewayCounters& counters() const noexcept { return counters_; }
  [[nodiscard]] std::size_t orders() const noexcept { return orders_.size(); }

  /// Token for counter n (exposed for tests).
  [[nodiscard]] ouch::Token token_for(std::uint64_t n) const noexcept;

 private:
  /// Order index for one of our tokens, or -1.
  [[nodiscard]] std::int64_t order_of(const ouch::Token& t) const noexcept;
  [[nodiscard]] std::int64_t order_of_client(std::uint64_t client_id) const noexcept;
  bool send_enter(OrderView& o, Transport& t);
  bool send_cancel(OrderView& o, Transport& t);
  bool send_replace(OrderView& o, Transport& t);
  void illegal() noexcept { ++counters_.illegal; }
  [[nodiscard]] std::uint32_t next_token(std::uint32_t order) noexcept;  // 0 = table full

  GatewayConfig cfg_;
  std::array<char, 6> prefix_{};
  std::vector<OrderView> orders_;            // by order index
  std::vector<std::uint32_t> token_order_;   // token counter - 1 -> order index
  std::vector<std::uint32_t> replacement_;   // order index -> pending replacement token counter (0 = none)
  struct PendingReplace {
    std::uint32_t shares{0};
    std::uint32_t price{0};
  };
  std::vector<PendingReplace> replace_args_;  // by order index
  std::vector<std::uint32_t> client_index_;   // open addressing: client_id -> order index + 1
  std::size_t client_mask_{0};
  GatewayCounters counters_{};
};

/// Reads the instance number stored in `path` (0 if absent), writes it back
/// incremented, and returns the value read: each process start of the day
/// gets a different instance, so tokens never repeat across restarts.
[[nodiscard]] std::uint32_t next_instance(const std::string& path);

}  // namespace lob::gateway
