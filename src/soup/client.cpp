#include "lob/soup/client.hpp"

#include <algorithm>

namespace lob::soup {

std::string_view to_string(ClientState s) noexcept {
  switch (s) {
    case ClientState::Disconnected: return "Disconnected";
    case ClientState::LoginSent: return "LoginSent";
    case ClientState::LoggedIn: return "LoggedIn";
    case ClientState::LogoutSent: return "LogoutSent";
    case ClientState::Rejected: return "Rejected";
    case ClientState::SessionEnded: return "SessionEnded";
    case ClientState::Failed: return "Failed";
  }
  return "?";
}

std::string_view to_string(CloseReason r) noexcept {
  switch (r) {
    case CloseReason::None: return "None";
    case CloseReason::Timeout: return "Timeout";
    case CloseReason::LoginTimeout: return "LoginTimeout";
    case CloseReason::ProtocolError: return "ProtocolError";
    case CloseReason::LoginRejected: return "LoginRejected";
    case CloseReason::EndOfSession: return "EndOfSession";
    case CloseReason::Logout: return "Logout";
    case CloseReason::SequenceMismatch: return "SequenceMismatch";
    case CloseReason::PeerClosed: return "PeerClosed";
    case CloseReason::SlowConsumer: return "SlowConsumer";
  }
  return "?";
}

ClientSession::ClientSession(const ClientConfig& cfg)
    : cfg_(cfg), out_(cfg.out_capacity), session_(cfg.session), next_seq_(cfg.first_sequence) {}

void ClientSession::on_connected(std::uint64_t now) {
  if (state_ != ClientState::Disconnected) return;  // terminal states stay; a live one cannot connect twice
  out_.clear();
  deframer_.reset();
  close_ = false;
  reason_ = CloseReason::None;
  last_recv_ = now;
  if (!put_login_request(out_, cfg_.user, cfg_.password, session_, next_seq_)) {
    close(CloseReason::ProtocolError, ClientState::Disconnected);  // out_capacity below one Login Request
    return;
  }
  last_send_ = now;
  state_ = ClientState::LoginSent;
}

void ClientSession::on_disconnected() {
  if (state_ == ClientState::LoginSent || state_ == ClientState::LoggedIn || state_ == ClientState::LogoutSent) {
    state_ = ClientState::Disconnected;
    if (reason_ == CloseReason::None) reason_ = CloseReason::PeerClosed;
  }
  out_.clear();
  deframer_.reset();
  close_ = false;
}

void ClientSession::close(CloseReason r, ClientState next) {
  reason_ = r;
  close_ = true;
  state_ = next;
}

bool ClientSession::put(char t, Bytes payload, std::uint64_t now) {
  if (!out_.append_packet(t, payload)) return false;
  last_send_ = now;
  return true;
}

bool ClientSession::handle(char t, Bytes payload, std::uint64_t now) {
  last_recv_ = now;
  if (t == type::kDebug) return false;  // ignored by protocol (§2.1)
  if (state_ == ClientState::LoginSent) {
    if (t == type::kLoginAccepted) {
      LoginAccepted a;
      if (!parse_login_accepted(payload, a)) {
        close(CloseReason::ProtocolError, ClientState::Disconnected);
        return false;
      }
      const bool session_ok = blank(session_) || same_field({a.session.data(), a.session.size()},
                                                            {session_.data(), session_.size()}, false);
      const bool seq_ok = next_seq_ == 0 || a.sequence == next_seq_;
      if (!session_ok || !seq_ok) {
        close(CloseReason::SequenceMismatch, ClientState::Failed);
        return false;
      }
      session_ = a.session;
      next_seq_ = a.sequence;
      state_ = ClientState::LoggedIn;
      ++logins_;
      return false;
    }
    if (t == type::kLoginRejected && payload.size() == 1) {
      close(CloseReason::LoginRejected, ClientState::Rejected);
      return false;
    }
    close(CloseReason::ProtocolError, ClientState::Disconnected);  // Login Accepted must come first (§2.2.1)
    return false;
  }
  if (state_ == ClientState::LoggedIn || state_ == ClientState::LogoutSent) {
    switch (t) {
      case type::kSequencedData:
        ++next_seq_;
        return true;
      case type::kServerHeartbeat:
        if (payload.empty()) return false;
        break;
      case type::kEndOfSession:
        if (payload.empty()) {
          close(CloseReason::EndOfSession, ClientState::SessionEnded);
          return false;
        }
        break;
      default:
        break;
    }
  }
  close(CloseReason::ProtocolError, ClientState::Disconnected);
  return false;
}

void ClientSession::on_timer(std::uint64_t now) {
  if (close_) return;
  const bool active = state_ == ClientState::LoginSent || state_ == ClientState::LoggedIn ||
                      state_ == ClientState::LogoutSent;
  if (!active) return;
  if (now >= last_recv_ + cfg_.timers.timeout_ns) {
    close(CloseReason::Timeout, ClientState::Disconnected);
    return;
  }
  if (state_ == ClientState::LoggedIn && out_.empty() && now >= last_send_ + cfg_.timers.heartbeat_ns) {
    (void)put(type::kClientHeartbeat, {}, now);
  }
}

bool ClientSession::send(Bytes payload, std::uint64_t now) {
  if (state_ != ClientState::LoggedIn || close_) return false;
  return put(type::kUnsequencedData, payload, now);
}

void ClientSession::logout(std::uint64_t now) {
  if (state_ != ClientState::LoggedIn || close_) return;
  if (put(type::kLogoutRequest, {}, now)) state_ = ClientState::LogoutSent;
}

std::uint64_t ClientSession::next_deadline() const noexcept {
  if (close_) return UINT64_MAX;
  if (state_ != ClientState::LoginSent && state_ != ClientState::LoggedIn && state_ != ClientState::LogoutSent) {
    return UINT64_MAX;
  }
  std::uint64_t d = last_recv_ + cfg_.timers.timeout_ns;
  if (state_ == ClientState::LoggedIn) d = std::min(d, last_send_ + cfg_.timers.heartbeat_ns);
  return d;
}

}  // namespace lob::soup
