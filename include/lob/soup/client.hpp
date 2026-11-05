#pragma once

// SoupBinTCP client session, sans-IO (L4.2). docs/design/L4_DESIGN.md §2.5.
//
//   Disconnected --on_connected--> LoginSent --'A'--> LoggedIn --logout()--> LogoutSent
//        ^                           |  'J' -> Rejected (terminal)     |  'Z' -> SessionEnded (terminal)
//        |                           |  timeout / protocol error      |  timeout / protocol error / mismatch
//        +--------- on_disconnected (keeps session id and next sequence number) --+
//
// Making the TCP connection is the driver's job; the state machine starts
// at on_connected(). "Closing" means the driver should close the socket:
// either at once (wants_close) or after flushing what is pending.
//
// Sequence numbers are implicit (§1.2): the client counts Sequenced Data
// packets. It logs in with the session it knows (blank the first time) and
// its next expected sequence number, so after a reconnect the server
// replays exactly what was missed. A Login Accepted whose sequence number
// differs from the one requested means the server is not where the client
// is (wrong session, or the server lost state): SequenceMismatch, close,
// and the client refuses to continue (terminal) -- guessing would corrupt
// the order state built on top [project convention, L4 design §2.5].

#include <cstdint>
#include <string>
#include <string_view>

#include "lob/soup/soup.hpp"

namespace lob::soup {

enum class ClientState : std::uint8_t { Disconnected, LoginSent, LoggedIn, LogoutSent, Rejected, SessionEnded, Failed };

enum class CloseReason : std::uint8_t {
  None,
  Timeout,           ///< nothing received for timeout_ns
  LoginTimeout,      ///< (server) no Login Request in time
  ProtocolError,     ///< framing error, unexpected or malformed packet
  LoginRejected,     ///< 'J' received / sent
  EndOfSession,      ///< 'Z' received / sent
  Logout,            ///< 'O' sent / received
  SequenceMismatch,  ///< (client) Login Accepted at a sequence number other than requested
  PeerClosed,        ///< the transport went away
  SlowConsumer,      ///< (server) the send buffer stayed full
};

[[nodiscard]] std::string_view to_string(ClientState s) noexcept;
[[nodiscard]] std::string_view to_string(CloseReason r) noexcept;

struct ClientConfig {
  std::string user;      ///< up to 6 characters
  std::string password;  ///< up to 10 characters
  SessionId session{field<10>("")};  ///< blank: the currently active session
  std::uint64_t first_sequence{1};   ///< sequence requested on the first login
  Timers timers{};
  std::size_t out_capacity{1U << 16};
};

class ClientSession {
 public:
  explicit ClientSession(const ClientConfig& cfg);

  /// The transport is up: queue the Login Request.
  void on_connected(std::uint64_t now);
  /// The transport went away (any reason). Keeps session and sequence.
  void on_disconnected();

  /// Received bytes. on_message(std::uint64_t seq, Bytes payload) for every
  /// Sequenced Data packet, in order.
  template <class F>
  void on_bytes(Bytes in, std::uint64_t now, F&& on_message);

  /// Time passes: heartbeat when idle, link timeout. Call at least at next_deadline().
  void on_timer(std::uint64_t now);

  /// Queue one Unsequenced Data packet. False when not logged in or the
  /// send buffer is full (the caller keeps the message).
  [[nodiscard]] bool send(Bytes payload, std::uint64_t now);
  /// Queue a Logout Request (LoggedIn only).
  void logout(std::uint64_t now);

  [[nodiscard]] ClientState state() const noexcept { return state_; }
  [[nodiscard]] CloseReason close_reason() const noexcept { return reason_; }
  /// The driver should close the socket now (after flushing pending()).
  [[nodiscard]] bool wants_close() const noexcept { return close_; }
  [[nodiscard]] const SessionId& session() const noexcept { return session_; }
  [[nodiscard]] std::uint64_t next_sequence() const noexcept { return next_seq_; }
  /// Earliest time on_timer has something to do (UINT64_MAX: nothing).
  [[nodiscard]] std::uint64_t next_deadline() const noexcept;
  [[nodiscard]] OutBuffer& out() noexcept { return out_; }
  [[nodiscard]] std::uint64_t logins() const noexcept { return logins_; }
  /// Server Heartbeats received (all connections). The first one after a
  /// login means the server had nothing left to send: the replay is over.
  [[nodiscard]] std::uint64_t heartbeats_received() const noexcept { return heartbeats_; }

 private:
  /// Returns true when the payload is a Sequenced Data message to deliver.
  bool handle(char t, Bytes payload, std::uint64_t now);
  void close(CloseReason r, ClientState next);
  bool put(char t, Bytes payload, std::uint64_t now);

  ClientConfig cfg_;
  OutBuffer out_;
  Deframer deframer_;
  ClientState state_{ClientState::Disconnected};
  CloseReason reason_{CloseReason::None};
  bool close_{false};
  SessionId session_;
  std::uint64_t next_seq_;
  std::uint64_t last_send_{0};
  std::uint64_t last_recv_{0};
  std::uint64_t logins_{0};
  std::uint64_t heartbeats_{0};
};

template <class F>
void ClientSession::on_bytes(Bytes in, std::uint64_t now, F&& on_message) {
  if (close_ || state_ == ClientState::Disconnected) return;
  const FrameError e = deframer_.feed(in, [&](char t, Bytes p) {
    if (handle(t, p, now)) on_message(next_seq_ - 1, p);
    return !close_;
  });
  if (e != FrameError::None && !close_) close(CloseReason::ProtocolError, ClientState::Disconnected);
}

}  // namespace lob::soup
