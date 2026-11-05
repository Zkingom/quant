#include "lob/soup/server.hpp"

#include <algorithm>
#include <string_view>

namespace lob::soup {

SessionLog::SessionLog(const SessionId& id, std::size_t max_messages, std::size_t max_bytes)
    : id_(id), bytes_(max_bytes), offsets_(max_messages + 1, 0) {}

std::uint64_t SessionLog::append(Bytes message) {
  const std::size_t start = offsets_[count_];
  if (ended_ || count_ + 1 >= offsets_.size() || start + message.size() > bytes_.size()) {
    ++refused_;
    return 0;
  }
  std::copy(message.begin(), message.end(), bytes_.begin() + static_cast<std::ptrdiff_t>(start));
  offsets_[count_ + 1] = static_cast<std::uint32_t>(start + message.size());
  return ++count_;
}

Bytes SessionLog::at(std::uint64_t seq) const noexcept {
  const std::size_t b = offsets_[seq - 1];
  const std::size_t e = offsets_[seq];
  return {bytes_.data() + b, e - b};
}

std::string_view to_string(ServerState s) noexcept {
  switch (s) {
    case ServerState::AwaitLogin: return "AwaitLogin";
    case ServerState::LoggedIn: return "LoggedIn";
    case ServerState::Closed: return "Closed";
  }
  return "?";
}

ServerConnection::ServerConnection(const ServerConfig& cfg, SessionLog& log, std::uint64_t now)
    : cfg_(cfg), log_(log), out_(cfg.out_capacity), accepted_at_(now), last_send_(now), last_recv_(now),
      last_drain_(now) {}

void ServerConnection::close(CloseReason r) noexcept {
  if (state_ == ServerState::Closed) return;
  reason_ = r;
  state_ = ServerState::Closed;
}

void ServerConnection::on_disconnected() { close(CloseReason::PeerClosed); }

bool ServerConnection::put(char t, Bytes payload, std::uint64_t now) {
  if (out_.empty()) last_drain_ = now;  // the slow-consumer clock starts when bytes start waiting
  if (!out_.append_packet(t, payload)) return false;
  last_send_ = now;
  return true;
}

void ServerConnection::consume(std::size_t n, std::uint64_t now) noexcept {
  if (n > 0) last_drain_ = now;
  out_.consume(n);
}

void ServerConnection::reject(char reason) {
  const std::uint8_t r = static_cast<std::uint8_t>(reason);
  (void)out_.append_packet(type::kLoginRejected, Bytes{&r, 1});
  close(CloseReason::LoginRejected);
}

void ServerConnection::login(Bytes payload, std::uint64_t now) {
  LoginRequest req;
  if (!parse_login_request(payload, req)) {
    close(CloseReason::ProtocolError);
    return;
  }
  const auto sv = [](const auto& a) { return std::string_view(a.data(), a.size()); };
  if (!same_field(sv(req.user), cfg_.user, true) || !same_field(sv(req.password), cfg_.password, true)) {
    reject(kRejectNotAuthorized);
    return;
  }
  const bool session_ok = blank(req.session) || same_field(sv(req.session), sv(log_.id()), false);
  if (!session_ok || log_.ended() || !login_allowed_) {
    reject(kRejectSessionNotAvailable);
    return;
  }
  const std::uint64_t end = log_.next_sequence();
  next_send_ = (req.sequence == 0 || req.sequence > end) ? end : req.sequence;
  // Login Accepted carries the session left-padded with spaces (§2.2.1).
  std::string_view id = sv(log_.id());
  while (!id.empty() && id.back() == ' ') id.remove_suffix(1);
  while (!id.empty() && id.front() == ' ') id.remove_prefix(1);
  if (out_.empty()) last_drain_ = now;
  if (!put_login_accepted(out_, field<10>(id, true), next_send_)) {
    close(CloseReason::SlowConsumer);  // out_capacity below one Login Accepted
    return;
  }
  last_send_ = now;
  state_ = ServerState::LoggedIn;
  pump(now);
}

bool ServerConnection::handle(char t, Bytes payload, std::uint64_t now) {
  last_recv_ = now;
  if (t == type::kDebug) return false;
  if (state_ == ServerState::AwaitLogin) {
    if (t == type::kLoginRequest) {
      login(payload, now);
    } else {
      close(CloseReason::ProtocolError);  // the first packet must be a Login Request (§2.3.1)
    }
    return false;
  }
  switch (t) {
    case type::kUnsequencedData:
      return true;
    case type::kClientHeartbeat:
      if (payload.empty()) return false;
      break;
    case type::kLogoutRequest:
      if (payload.empty()) {
        close(CloseReason::Logout);  // "immediately terminate the connection" (§2.3.4)
        return false;
      }
      break;
    default:
      break;
  }
  close(CloseReason::ProtocolError);
  return false;
}

void ServerConnection::pump(std::uint64_t now) {
  if (state_ != ServerState::LoggedIn) return;
  while (next_send_ < log_.next_sequence()) {
    if (!put(type::kSequencedData, log_.at(next_send_), now)) return;  // full: continue after consume()
    ++next_send_;
  }
  if (log_.ended() && !end_sent_ && put(type::kEndOfSession, {}, now)) {
    end_sent_ = true;
    close(CloseReason::EndOfSession);
  }
}

void ServerConnection::on_timer(std::uint64_t now) {
  if (state_ == ServerState::Closed) return;
  if (state_ == ServerState::AwaitLogin) {
    if (now >= accepted_at_ + cfg_.timers.login_timeout_ns) close(CloseReason::LoginTimeout);
    return;
  }
  if (now >= last_recv_ + cfg_.timers.timeout_ns) {
    close(CloseReason::Timeout);
    return;
  }
  if (!out_.empty() && now >= last_drain_ + cfg_.timers.timeout_ns) {
    close(CloseReason::SlowConsumer);
    return;
  }
  pump(now);
  if (state_ == ServerState::LoggedIn && out_.empty() && now >= last_send_ + cfg_.timers.heartbeat_ns) {
    (void)put(type::kServerHeartbeat, {}, now);
  }
}

std::uint64_t ServerConnection::next_deadline() const noexcept {
  if (state_ == ServerState::Closed) return UINT64_MAX;
  if (state_ == ServerState::AwaitLogin) return accepted_at_ + cfg_.timers.login_timeout_ns;
  std::uint64_t d = std::min(last_recv_ + cfg_.timers.timeout_ns, last_send_ + cfg_.timers.heartbeat_ns);
  if (!out_.empty()) d = std::min(d, last_drain_ + cfg_.timers.timeout_ns);
  return d;
}

}  // namespace lob::soup
