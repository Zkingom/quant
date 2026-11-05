#pragma once

// Network order entry behind TradingStage (L4.5). docs/design/L4_DESIGN.md §4.
//
//   ClientConn        a SoupBinTCP client session on a real socket; also the
//                     gateway's Transport (counts the messages it sends)
//   LockstepExchange  TradingStage's `Ex` for the LOCKSTEP mode: every step
//                     waits for quiescence, so the orders / fills / position
//                     digests must equal the in-process SimExchange's, bit
//                     for bit (§4.1)
//   FreeRunExchange   TradingStage's `Ex` for the FREE-RUN mode: nothing
//                     waits; reports are delivered when they happen to have
//                     arrived. Not comparable by hash -- checked by
//                     invariants, and RECORDED (ClientJournal) so that the
//                     run replays offline bit for bit (§4.2)
//   ReplayExchange    the offline replay of a FreeRunExchange recording
//   NetStage          pipeline downstream: tells the exchange about each
//                     input before TradingStage sees its events
//
// Quiescence (lockstep): after a step (a market event on the control queue,
// or an order on TCP), post Barrier{account, inbound = OUCH messages sent so
// far}. The server answers once it has processed that many inbound messages
// of the account, with its next outbound sequence number K; the client reads
// TCP until its own next expected sequence number is K. What it read is
// exactly this step's output. (Waiting for "the next message" would hang:
// a superfluous cancel gets no answer at all, by specification.)
//
// Nesting: the in-process exchange computes a market event's fills before
// the strategy sees the first of them, and a request the strategy sends in
// reaction is answered before the next fill is delivered. on_market() reads
// the whole step into the report batch first, and submit() reads only what
// comes after it -- TradingStage delivers them in the same order.

#include <cstdint>
#include <span>
#include <vector>

#include "lob/gateway/gateway.hpp"
#include "lob/md/feed.hpp"
#include "lob/net/epoll_server.hpp"
#include "lob/pipeline/pipeline.hpp"
#include "lob/soup/client.hpp"
#include "lob/soup/server.hpp"

namespace lob::net {

class ClientConn final : public gateway::Transport {
 public:
  explicit ClientConn(const soup::ClientConfig& cfg);
  ~ClientConn() override;
  ClientConn(const ClientConn&) = delete;
  ClientConn& operator=(const ClientConn&) = delete;

  /// Blocking connect to 127.0.0.1:port, then non-blocking; queues the login.
  bool connect(std::uint16_t port);
  /// Closes the socket (the session keeps its sequence number).
  void close();
  /// Queues one Unsequenced Data packet and flushes. Never fails while
  /// logged in: a full send buffer is flushed (waiting) first.
  bool send(ouch::Bytes payload) override;

  /// Waits up to timeout_ms for data (0: only what is there), reads what is
  /// available, runs timers, flushes; calls f(seq, payload) per sequenced
  /// message. False when the connection is (or became) closed.
  template <class F>
  bool poll(int timeout_ms, F&& f);

  [[nodiscard]] soup::ClientSession& session() noexcept { return s_; }
  /// Messages sent on the CURRENT connection (barriers count per connection:
  /// what was in flight on a dropped connection may never be processed).
  [[nodiscard]] std::uint64_t sent() const noexcept { return sent_; }
  [[nodiscard]] bool connected() const noexcept { return fd_ >= 0; }
  [[nodiscard]] std::uint64_t heartbeats() const noexcept { return s_.heartbeats_received(); }
  /// TSC (pipeline::ticks) right after the last send() returned (L4.6).
  [[nodiscard]] std::uint64_t last_send_tsc() const noexcept { return last_send_tsc_; }

 private:
  /// Reads once into rbuf_; returns bytes read, 0 = nothing, -1 = closed.
  long read_some(int timeout_ms);
  void flush(bool wait);

  soup::ClientSession s_;
  int fd_{-1};
  std::uint64_t sent_{0};
  std::uint64_t last_send_tsc_{0};
  std::vector<std::uint8_t> rbuf_;
};

template <class F>
bool ClientConn::poll(int timeout_ms, F&& f) {
  if (fd_ < 0) return false;
  const long n = read_some(timeout_ms);
  if (n < 0) {
    close();
    return false;
  }
  const std::uint64_t now = monotonic_ns();
  if (n > 0) s_.on_bytes({rbuf_.data(), static_cast<std::size_t>(n)}, now, f);
  s_.on_timer(now);
  flush(false);
  if (s_.wants_close()) {
    close();
    return false;
  }
  return true;
}

struct NetStats {
  std::uint64_t barriers{0};
  std::uint64_t events_sent{0};
  std::uint64_t events_skipped{0};  ///< no open order: nothing can fill
  std::uint64_t messages{0};        ///< outbound messages handed to the gateway
  std::uint64_t reconnects{0};
};

class LockstepExchange {
 public:
  LockstepExchange(EpollServer& server, ClientConn& conn, gateway::OrderGateway& gw, std::uint32_t account,
                   int timeout_ms = 20'000)
      : srv_(&server), conn_(&conn), gw_(&gw), account_(account), timeout_ms_(timeout_ms) {}

  /// Before TradingStage sees an input: the exchange applies it to its book.
  void begin_input(const md::FeedInput& in, std::span<const md::FeedEvent> events);
  void submit(const strategy::OrderRequest& r, const md::Book& book, sim::Reports& out);
  void on_market(const md::FeedEvent& e, sim::Reports& out);
  std::size_t open_ids(std::span<std::uint64_t> out) const { return gw_->open_ids(out); }
  [[nodiscard]] bool inconsistent() const noexcept { return gw_->inconsistent() || failed_; }
  [[nodiscard]] bool failed() const noexcept { return failed_; }
  [[nodiscard]] const NetStats& stats() const noexcept { return st_; }

 private:
  void quiesce(sim::Reports& out);

  EpollServer* srv_;
  ClientConn* conn_;
  gateway::OrderGateway* gw_;
  std::uint32_t account_;
  int timeout_ms_;
  const md::FeedEvent* base_{nullptr};
  std::uint64_t next_barrier_{1};
  bool failed_{false};
  NetStats st_{};
};

/// What the free-run client saw, as a sequence of operations: each
/// on_market call, then within it a disconnect, deliveries of the next n
/// outbound messages, the end of a replay; finally the drain.
struct ClientJournal {
  enum Op : std::uint32_t { kCall = 0, kDeliver = 1, kDisconnect = 2, kReplayComplete = 3, kDrain = 4 };
  std::uint64_t first_seq{1};      ///< the first message the recording covers
  std::vector<std::uint32_t> ops;  ///< (op << 28) | value
  void push(Op op, std::uint32_t value = 0) { ops.push_back((static_cast<std::uint32_t>(op) << 28U) | value); }
};

class FreeRunExchange {
 public:
  FreeRunExchange(EpollServer& server, ClientConn& conn, gateway::OrderGateway& gw, std::uint32_t account,
                  ClientJournal* journal, int timeout_ms = 20'000)
      : srv_(&server), conn_(&conn), gw_(&gw), account_(account), journal_(journal), timeout_ms_(timeout_ms) {
    if (journal_ != nullptr) journal_->first_seq = conn.session().next_sequence();
  }

  void begin_input(const md::FeedInput& in, std::span<const md::FeedEvent> events);
  void submit(const strategy::OrderRequest& r, const md::Book& book, sim::Reports& out);
  void on_market(const md::FeedEvent& e, sim::Reports& out);
  std::size_t open_ids(std::span<std::uint64_t> out) const { return gw_->open_ids(out); }
  [[nodiscard]] bool inconsistent() const noexcept { return gw_->inconsistent() || failed_; }

  /// Fault injection: drop the connection at the start of these on_market
  /// calls (1-based, ascending), then reconnect, resend, wait for the replay.
  void drop_at(std::vector<std::uint64_t> calls) { drops_ = std::move(calls); }
  /// End of the run: cancel everything, wait for quiescence, deliver the rest.
  void drain(sim::Reports& out);

  /// Client view from every report this adapter produced (drain included).
  [[nodiscard]] std::int64_t position() const noexcept { return position_; }
  [[nodiscard]] std::int64_t cash() const noexcept { return cash_; }
  [[nodiscard]] std::int64_t peak_exposure() const noexcept { return peak_exposure_; }
  [[nodiscard]] bool failed() const noexcept { return failed_; }
  /// Where it failed: "connect", "login", "replay", "drain" (empty: no failure).
  [[nodiscard]] const char* failure() const noexcept { return failure_; }
  [[nodiscard]] const NetStats& stats() const noexcept { return st_; }

 private:
  void fail(const char* where) noexcept {
    failed_ = true;
    if (failure_[0] == '\0') failure_ = where;
  }
  std::uint32_t deliver_available(int timeout_ms, sim::Reports& out);
  void account_reports(const sim::Reports& out, std::size_t from);
  void reconnect(sim::Reports& out);

  EpollServer* srv_;
  ClientConn* conn_;
  gateway::OrderGateway* gw_;
  std::uint32_t account_;
  ClientJournal* journal_;
  int timeout_ms_;
  std::vector<std::uint64_t> drops_;
  std::size_t next_drop_{0};
  std::uint64_t calls_{0};
  std::uint64_t next_barrier_{1};
  bool failed_{false};
  const char* failure_{""};
  std::int64_t position_{0};
  std::int64_t cash_{0};
  std::int64_t open_buy_{0};
  std::int64_t open_sell_{0};
  std::int64_t peak_exposure_{0};
  NetStats st_{};
};

/// Offline replay of a FreeRunExchange recording: the same gateway
/// configuration, the account's outbound messages (the server's log), the
/// client journal. TradingStage over it must reproduce the live digests.
class ReplayExchange {
 public:
  ReplayExchange(gateway::OrderGateway& gw, const soup::SessionLog& log, const ClientJournal& j)
      : gw_(&gw), log_(&log), j_(&j), next_msg_(j.first_seq) {}

  void begin_input(const md::FeedInput&, std::span<const md::FeedEvent>) {}
  void submit(const strategy::OrderRequest& r, const md::Book& book, sim::Reports& out);
  void on_market(const md::FeedEvent& e, sim::Reports& out);
  std::size_t open_ids(std::span<std::uint64_t> out) const { return gw_->open_ids(out); }
  [[nodiscard]] bool inconsistent() const noexcept { return gw_->inconsistent() || failed_; }
  /// True when the replay consumed the journal up to the drain.
  [[nodiscard]] bool complete() const noexcept;
  [[nodiscard]] bool failed() const noexcept { return failed_; }

 private:
  struct Null final : gateway::Transport {
    bool send(ouch::Bytes) override { return true; }
  };
  gateway::OrderGateway* gw_;
  const soup::SessionLog* log_;
  const ClientJournal* j_;
  Null null_;
  std::size_t op_{0};
  std::uint64_t next_msg_;
  bool failed_{false};
};

/// Pipeline downstream: the exchange learns about every input before
/// TradingStage processes its events (lockstep / free-run / replay).
template <class Stage>
struct NetStage {
  Stage& stage;
  void on_feed(const md::FeedInput& in, std::span<const md::FeedEvent> ev, const md::FeedHandler& f, std::uint64_t t,
               pipeline::LatencyRecorder* rec, bool sampled) {
    stage.exchange().begin_input(in, ev);
    stage.on_feed(in, ev, f, t, rec, sampled);
  }
};

}  // namespace lob::net
