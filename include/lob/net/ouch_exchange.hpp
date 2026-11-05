#pragma once

// Simulated exchange core: OUCH 4.2 order entry on the existing simulated
// matching (L4.3). sans-IO -- a pure function of the calls it receives;
// the epoll driver (epoll_server.hpp) only moves bytes. L4 design §2.6.
//
//   market data   apply_input / market_event: the exchange's OWN
//                 FeedHandler (its view of the public book: the conservative
//                 fill model reads queue positions from it), then
//                 SimExchange::on_market -> fills of resting orders
//   order entry   on_message(account, OUCH inbound payload)
//                 -> SimExchange::submit / cancel / reduce
//                    (inside: the unchanged L1 MatchingEngine)
//   output        sequenced OUCH outbound messages appended to the
//                 account's SoupBinTCP SessionLog
//
// Specification behaviour implemented (OUCH 4.2 §2):
//   - a used token is ignored (Enter), and so is a used replacement token;
//   - Cancel carries the intended order size: >= open shares is
//     superfluous (ignored), 0 cancels all, otherwise the order is reduced;
//   - Replace: existing not live -> ignored; invalid details -> the
//     existing order is cancelled and the replacement token is NOT
//     consumed; valid -> Replaced (new token, new reference number, loses
//     priority), Shares = total liable for the chain minus executed;
//   - Rejected consumes the token.
// Subset policy of this simulator [project conventions]: one instrument
// (the feed's symbol; other stock -> Rejected 'S'); sides B / S only
// (short sales -> 'O'); limit orders only (price 0, market prices or an
// unknown symbol price -> 'X'); display A or Y; TIF 0 (IOC), 99998, 99999;
// minimum quantity 0 ('N'); cross type N ('R'). Trading-phase checks are
// the client's pre-trade risk (D-054), not done here.
// Partial cancel keeps the order's place in the PUBLIC queue (conservative
// model state) but re-enters it behind our own orders at that price (the
// L1 engine has no reduce; SimExchange::reduce).
// Exchange-side throttle: per-account token bucket on Enter / Replace,
// excess -> Rejected 'x' (Port Message Rate Restriction); cancels are never
// throttled. Cancel-on-disconnect: Canceled 'Z' for every live order of the
// account, in ascending order id, sequenced like any other message.
// Outbound timestamps are the exchange's market time (the last ITCH
// timestamp it applied): deterministic.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "lob/md/feed.hpp"
#include "lob/ouch/ouch.hpp"
#include "lob/sim/sim_exchange.hpp"
#include "lob/soup/server.hpp"

namespace lob::net {

struct AccountConfig {
  std::string user;
  std::string password;
  std::string firm{"ZKQT"};
  bool cancel_on_disconnect{true};
  std::uint32_t throttle_per_sec{0};  ///< Enter + Replace per second; 0 = unlimited
  std::uint32_t throttle_burst{1};    ///< bucket size
};

struct ExchangeConfig {
  md::FeedConfig feed{};
  sim::SimConfig sim{};
  std::vector<AccountConfig> accounts;
  soup::SessionId session{soup::field<10>("SIM0000001")};
  std::size_t log_messages{1U << 20};  ///< per account
  std::size_t log_bytes{1U << 26};     ///< per account
  std::size_t max_orders{1U << 16};    ///< tokens per day, all accounts
  /// Verification (L4.5): recompute each account's exposure whenever it can
  /// grow (accept / replace) -- O(orders) per order, off by default.
  bool track_exposure{false};
};

struct ExchangeCounters {
  std::uint64_t inbound{0};
  std::uint64_t invalid_inbound{0};  ///< not Decoded (garbage, Skipped Modify)
  std::uint64_t enters{0};
  std::uint64_t replaces{0};
  std::uint64_t cancels{0};
  std::uint64_t duplicate_tokens{0};  ///< Enter / Replace with a used token: ignored
  std::uint64_t ignored_cancels{0};   ///< not live, or superfluous
  std::uint64_t ignored_replaces{0};
  std::uint64_t accepted{0};
  std::uint64_t rejected{0};
  std::uint64_t throttled{0};
  std::uint64_t executions{0};
  std::uint64_t executed_shares{0};
  std::uint64_t cancelled_on_disconnect{0};
  std::uint64_t order_table_full{0};
  std::uint64_t log_refused{0};  ///< outbound message lost: session log full
};

/// Per-account economics as the exchange sees them (verification only).
struct AccountPosition {
  std::int64_t position{0};
  std::int64_t cash{0};  ///< price units x shares
  std::uint64_t open_buy{0};
  std::uint64_t open_sell{0};
  std::uint64_t live_orders{0};
  /// Largest max(position + open buy, open sell - position) seen after any
  /// change (the pre-trade risk bound, checked from the exchange's side).
  std::int64_t peak_exposure{0};
};

class OuchExchange {
 public:
  static constexpr std::size_t npos = static_cast<std::size_t>(-1);

  explicit OuchExchange(const ExchangeConfig& cfg);
  OuchExchange(const OuchExchange&) = delete;
  OuchExchange& operator=(const OuchExchange&) = delete;

  // --- market data -----------------------------------------------------------
  /// Applies one ITCH input to the exchange's book; returns the number of
  /// feed events it produced (market_event indexes them).
  std::size_t apply_input(const md::FeedInput& in);
  /// Fills caused by event k of the last applied input (no-op unless a BookUpdate).
  void market_event(std::size_t k);
  /// apply_input + every event.
  void on_market(const md::FeedInput& in);

  // --- order entry ------------------------------------------------------------
  void on_message(std::size_t account, soup::Bytes payload, std::uint64_t now_ns);
  /// The account's connection went away (cancel-on-disconnect if configured).
  void on_disconnect(std::size_t account);
  /// End of the trading day: End of Session on every account's log.
  void end_of_day();

  // --- accounts / sessions ---------------------------------------------------
  [[nodiscard]] std::size_t accounts() const noexcept { return accounts_.size(); }
  [[nodiscard]] std::size_t find_account(std::string_view user) const noexcept;
  [[nodiscard]] const AccountConfig& account(std::size_t a) const { return cfg_.accounts.at(a); }
  [[nodiscard]] soup::SessionLog& log(std::size_t a) { return *accounts_.at(a).log; }
  [[nodiscard]] const soup::SessionLog& log(std::size_t a) const { return *accounts_.at(a).log; }

  // --- verification ------------------------------------------------------------
  [[nodiscard]] AccountPosition position(std::size_t a) const;
  /// FNV-1a over every outbound message of the account, in sequence order.
  [[nodiscard]] std::uint64_t outbound_hash(std::size_t a) const;
  [[nodiscard]] const ExchangeCounters& counters() const noexcept { return c_; }
  [[nodiscard]] const md::FeedHandler& feed() const noexcept { return feed_; }
  [[nodiscard]] const sim::SimExchange& sim() const noexcept { return sim_; }
  [[nodiscard]] std::uint64_t market_time() const noexcept { return market_ts_; }

 private:
  struct Order {
    std::uint16_t account{0};
    ouch::Token token{};
    sim::Side side{sim::Side::Buy};
    std::uint32_t price{0};
    std::uint32_t open{0};      ///< shares on the book
    std::uint32_t executed{0};  ///< cumulative over the replace chain
    std::uint32_t tif{0};
    char display{'A'};
    std::uint64_t ref{0};       ///< Order Reference Number
    bool live{false};
  };
  struct Account {
    std::unique_ptr<soup::SessionLog> log;
    std::uint64_t bucket{0};  ///< throttle level in 1e-9 tokens (integer: deterministic)
    std::uint64_t last_refill{0};
    bool primed{false};
    AccountPosition pos{};
  };

  void on_enter(std::size_t a, const ouch::EnterOrder& m, std::uint64_t now);
  void on_replace(std::size_t a, const ouch::ReplaceOrder& m, std::uint64_t now);
  void on_cancel(std::size_t a, const ouch::CancelOrder& m);
  /// Subset policy for a new order / replacement; 0 = acceptable, else a Rejected reason.
  [[nodiscard]] char policy(std::uint32_t price, std::uint32_t tif, char display, std::uint32_t min_qty) const;
  [[nodiscard]] bool throttle(std::size_t a, std::uint64_t now);
  void reject(std::size_t a, const ouch::Token& t, char reason);
  /// Appends an outbound message to the account's log.
  template <class M>
  void emit(std::size_t a, const M& m);
  /// Translates SimExchange reports (fills, cancels, rejects) into OUCH.
  void deliver(const sim::Reports& r, char cancel_reason);
  void cancel_order(std::uint32_t idx, char reason);
  void track_exposure(std::size_t a);

  // token table: (account, token) -> order index + 1, open addressing, never erased
  [[nodiscard]] std::size_t slot_of(std::size_t a, const ouch::Token& t) const noexcept;
  [[nodiscard]] std::uint32_t find_token(std::size_t a, const ouch::Token& t) const noexcept;
  [[nodiscard]] std::uint32_t new_order(std::size_t a, const ouch::Token& t);

  ExchangeConfig cfg_;
  md::FeedHandler feed_;
  sim::SimExchange sim_;
  std::vector<Account> accounts_;
  std::vector<Order> orders_;
  std::vector<std::uint32_t> table_;
  std::size_t mask_{0};
  std::vector<md::FeedEvent> events_;
  sim::Reports reports_;
  std::uint64_t market_ts_{0};
  std::uint64_t next_ref_{1};
  std::uint64_t next_match_{1};
  ExchangeCounters c_{};
};

}  // namespace lob::net
