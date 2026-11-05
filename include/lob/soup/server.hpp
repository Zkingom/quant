#pragma once

// SoupBinTCP server side, sans-IO (L4.2). docs/design/L4_DESIGN.md §2.5.
//
// SessionLog: one logical session (one OUCH account, one day). It outlives
// connections: every sequenced message is kept so that a client logging in
// again with its next expected sequence number gets exactly what it missed.
// Fixed capacity, reserved at construction; append() refuses when full.
//
// ServerConnection: one TCP connection.
//
//   AwaitLogin --valid 'L'--> LoggedIn --'O' / timeout / protocol error / slow consumer--> Closed
//      |  bad credentials -> 'J' 'A' -> Closed        |  session ended and fully sent -> 'Z' -> Closed
//      |  session unknown / ended / busy -> 'J' 'S' -> Closed
//      |  login timeout, first packet not 'L', protocol error -> Closed
//
// Sequenced messages are PULLED from the log into the send buffer while it
// has room (pump), so a long replay never grows memory: a client that does
// not read for timeout_ns is closed as a slow consumer.
//
// Requested sequence number [project conventions, L4 design §2.5]: 0 =
// only messages generated from now on; beyond the end of the log = start
// at the end (Login Accepted states the number actually used, and the
// client detects the mismatch).

#include <cstdint>
#include <string>
#include <vector>

#include "lob/soup/client.hpp"  // CloseReason
#include "lob/soup/soup.hpp"

namespace lob::soup {

class SessionLog {
 public:
  SessionLog(const SessionId& id, std::size_t max_messages, std::size_t max_bytes);

  /// Sequence number of the appended message, or 0 when the log is full or ended.
  std::uint64_t append(Bytes message);
  /// Precondition: 1 <= seq < next_sequence().
  [[nodiscard]] Bytes at(std::uint64_t seq) const noexcept;
  [[nodiscard]] std::uint64_t next_sequence() const noexcept { return count_ + 1; }
  [[nodiscard]] const SessionId& id() const noexcept { return id_; }
  /// No message is appended after end(); connections send End of Session
  /// once they have sent everything.
  void end() noexcept { ended_ = true; }
  [[nodiscard]] bool ended() const noexcept { return ended_; }
  [[nodiscard]] std::uint64_t refused() const noexcept { return refused_; }

 private:
  SessionId id_;
  std::vector<std::uint8_t> bytes_;
  std::vector<std::uint32_t> offsets_;  // offsets_[i] = start of message i+1; offsets_[count_] = end
  std::uint64_t count_{0};
  std::uint64_t refused_{0};
  bool ended_{false};
};

enum class ServerState : std::uint8_t { AwaitLogin, LoggedIn, Closed };

[[nodiscard]] std::string_view to_string(ServerState s) noexcept;

struct ServerConfig {
  std::string user;
  std::string password;
  Timers timers{};
  std::size_t out_capacity{1U << 16};
};

class ServerConnection {
 public:
  ServerConnection(const ServerConfig& cfg, SessionLog& log, std::uint64_t now);

  /// The server refuses a second connection of the same account: set false
  /// while another connection is logged in ('J' 'S').
  void set_login_allowed(bool allowed) noexcept { login_allowed_ = allowed; }

  /// Received bytes; on_message(Bytes payload) for every Unsequenced Data packet.
  template <class F>
  void on_bytes(Bytes in, std::uint64_t now, F&& on_message);
  /// Timeouts, heartbeat, and pump(). Call at least at next_deadline().
  void on_timer(std::uint64_t now);
  /// Copies log messages into the send buffer while it has room; End of
  /// Session after the last one if the log has ended.
  void pump(std::uint64_t now);
  /// The transport went away.
  void on_disconnected();

  [[nodiscard]] Bytes pending() const noexcept { return out_.pending(); }
  void consume(std::size_t n, std::uint64_t now) noexcept;

  [[nodiscard]] ServerState state() const noexcept { return state_; }
  [[nodiscard]] CloseReason close_reason() const noexcept { return reason_; }
  /// The driver should close the socket (after one attempt to flush pending()).
  [[nodiscard]] bool wants_close() const noexcept { return state_ == ServerState::Closed; }
  /// Next sequence number this connection will send.
  [[nodiscard]] std::uint64_t next_to_send() const noexcept { return next_send_; }
  [[nodiscard]] std::uint64_t next_deadline() const noexcept;
  [[nodiscard]] const SessionLog& log() const noexcept { return log_; }

 private:
  bool handle(char t, Bytes payload, std::uint64_t now);
  void login(Bytes payload, std::uint64_t now);
  void reject(char reason);
  void close(CloseReason r) noexcept;
  bool put(char t, Bytes payload, std::uint64_t now);

  ServerConfig cfg_;
  SessionLog& log_;
  OutBuffer out_;
  Deframer deframer_;
  ServerState state_{ServerState::AwaitLogin};
  CloseReason reason_{CloseReason::None};
  bool login_allowed_{true};
  std::uint64_t accepted_at_;
  std::uint64_t last_send_;
  std::uint64_t last_recv_;
  std::uint64_t last_drain_;
  std::uint64_t next_send_{0};
  bool end_sent_{false};
};

template <class F>
void ServerConnection::on_bytes(Bytes in, std::uint64_t now, F&& on_message) {
  if (state_ == ServerState::Closed) return;
  const FrameError e = deframer_.feed(in, [&](char t, Bytes p) {
    if (handle(t, p, now)) on_message(p);
    return state_ != ServerState::Closed;
  });
  if (e != FrameError::None && state_ != ServerState::Closed) close(CloseReason::ProtocolError);
}

}  // namespace lob::soup
