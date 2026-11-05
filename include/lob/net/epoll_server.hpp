#pragma once

// Single-threaded epoll driver for the simulated exchange (L4.3). It only
// moves bytes and time: every protocol decision is in the sans-IO
// soup::ServerConnection and net::OuchExchange. L4 design §2.6.
//
//   listen socket   accept -> TCP_NODELAY, non-blocking; the first logical
//                   packet (Login Request) names the account, then a
//                   ServerConnection bound to that account's SessionLog
//                   takes over (unknown user -> Login Rejected 'A'); a
//                   successful login closes the account's older connection
//                   (takeover: the client reconnected before the server saw
//                   the drop; cancel-on-disconnect runs first)
//   connections     level-triggered EPOLLIN; output flushed after every
//                   event, EPOLLOUT only while the kernel buffer is full
//   control queue   SPSC + eventfd from ONE producer thread (the market
//                   side / the lockstep coordinator): market inputs and
//                   events, barriers, end of day, stop
//   timers          epoll_wait's timeout is the earliest connection deadline
//
// Barrier {account, n}: answered (reply queue + reply eventfd) once the
// exchange has processed n inbound OUCH messages of that account, with the
// account's next outbound sequence number at that moment -- the lockstep
// "quiescence" watermark (L4 design §4.1). The control queue and TCP are
// different channels; counting inbound messages is what orders them.
//
// Market inputs: auto_market_events = true applies every event of an input
// at once (free-run); false waits for one Event control per event (lockstep,
// same granularity as the in-process TradingStage).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "lob/concurrent/spsc_queue.hpp"
#include "lob/md/feed.hpp"
#include "lob/net/ouch_exchange.hpp"
#include "lob/soup/server.hpp"

namespace lob::net {

struct Control {
  enum class Kind : std::uint8_t { Input, Event, Barrier, EndOfDay, Stop };
  Kind kind{Kind::Input};
  md::FeedInput in{};          ///< Input
  std::uint32_t event{0};      ///< Event: index into the last input's events
  std::uint32_t account{0};    ///< Barrier
  std::uint64_t inbound{0};    ///< Barrier: inbound messages of the account sent so far
  std::uint64_t id{0};         ///< Barrier: echoed in the reply
};
static_assert(std::is_trivially_copyable_v<Control>);

struct ControlReply {
  std::uint64_t id{0};
  std::uint64_t next_sequence{0};  ///< the account's next outbound sequence number
};

struct ServerOptions {
  std::string address{"127.0.0.1"};
  std::uint16_t port{0};  ///< 0: an ephemeral port (see EpollServer::port())
  soup::Timers timers{};
  std::size_t out_capacity{1U << 16};
  std::size_t control_capacity{1U << 12};
  bool auto_market_events{true};
};

struct ServerStats {
  std::uint64_t accepted{0};
  std::uint64_t closed{0};
  std::uint64_t logins{0};
  std::uint64_t login_rejects{0};
  std::uint64_t inbound{0};
  std::uint64_t bytes_in{0};
  std::uint64_t bytes_out{0};
  std::uint64_t would_block{0};  ///< send() hit a full kernel buffer
  std::uint64_t barriers{0};
};

/// Called for every inbound OUCH payload before the exchange processes it
/// (tick-to-trade measurement, L4.6). Plain function pointer: no allocation.
using InboundHook = void (*)(void* ctx, std::size_t account, soup::Bytes payload);

class EpollServer {
 public:
  EpollServer(OuchExchange& ex, const ServerOptions& opt);
  ~EpollServer();
  EpollServer(const EpollServer&) = delete;
  EpollServer& operator=(const EpollServer&) = delete;

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

  // --- control channel (one producer thread) ---------------------------------
  [[nodiscard]] bool post(const Control& c);
  [[nodiscard]] bool poll_reply(ControlReply& out) { return replies_.try_pop(out); }
  /// Becomes readable when a reply is queued (poll / epoll on it to wait).
  [[nodiscard]] int reply_fd() const noexcept { return reply_fd_; }

  /// Runs until a Stop control arrives.
  void run();
  /// One epoll_wait (at most `max_wait_ms`, earlier at the next deadline);
  /// false once stopped.
  bool run_once(int max_wait_ms);

  void set_inbound_hook(InboundHook h, void* ctx) noexcept {
    hook_ = h;
    hook_ctx_ = ctx;
  }
  [[nodiscard]] const ServerStats& stats() const noexcept { return st_; }
  /// Logged-in connections right now.
  [[nodiscard]] std::size_t logged_in() const noexcept;

 private:
  struct Conn {
    int fd{-1};
    std::size_t account{OuchExchange::npos};
    std::unique_ptr<soup::ServerConnection> session;  // null until the first packet names the account
    std::vector<std::uint8_t> pre;                    // bytes before that
    std::uint64_t opened{0};
    bool want_out{false};
  };

  void accept_all(std::uint64_t now);
  void on_readable(Conn& c, std::uint64_t now);
  void bind_account(Conn& c, std::uint64_t now);
  void feed(Conn& c, soup::Bytes in, std::uint64_t now);
  void drain_control();
  void answer_barriers();
  void pump_all(std::uint64_t now);
  void flush(Conn& c, std::uint64_t now);
  void close_conn(Conn& c);
  void sweep_closed();
  [[nodiscard]] std::uint64_t next_deadline() const noexcept;

  OuchExchange& ex_;
  ServerOptions opt_;
  int epfd_{-1};
  int listen_fd_{-1};
  int control_fd_{-1};
  int reply_fd_{-1};
  std::uint16_t port_{0};
  bool stopped_{false};
  concurrent::SpscQueue<Control> control_;
  concurrent::SpscQueue<ControlReply> replies_;
  std::vector<std::unique_ptr<Conn>> conns_;
  std::vector<Control> barriers_;            // waiting for inbound messages
  std::vector<std::uint64_t> processed_;     // inbound messages processed, per account
  std::vector<std::uint8_t> rbuf_;
  soup::SessionLog reject_log_;              // for connections of unknown users (always rejected)
  InboundHook hook_{nullptr};
  void* hook_ctx_{nullptr};
  ServerStats st_{};
};

/// CLOCK_MONOTONIC in ns.
[[nodiscard]] std::uint64_t monotonic_ns() noexcept;

}  // namespace lob::net
