#include "lob/net/lockstep.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <thread>

#include "lob/pipeline/clock.hpp"

namespace lob::net {

namespace {
std::uint64_t ms_to_ns(int ms) noexcept {
  return static_cast<std::uint64_t>(ms < 0 ? 0 : ms) * std::uint64_t{1'000'000};
}
}  // namespace

// --- ClientConn ----------------------------------------------------------------------

ClientConn::ClientConn(const soup::ClientConfig& cfg) : s_(cfg), rbuf_(1U << 16) {}

ClientConn::~ClientConn() { close(); }

bool ClientConn::connect(std::uint16_t port) {
  close();
  fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd_ < 0) return false;
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
  if (::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
    ::close(fd_);
    fd_ = -1;
    return false;
  }
  const int yes = 1;
  ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof yes);
  ::fcntl(fd_, F_SETFL, ::fcntl(fd_, F_GETFL) | O_NONBLOCK);
  sent_ = 0;
  s_.on_connected(monotonic_ns());
  flush(true);
  return true;
}

void ClientConn::close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
    s_.on_disconnected();
  }
}

void ClientConn::flush(bool wait) {
  while (fd_ >= 0 && !s_.out().empty()) {
    const auto p = s_.out().pending();
    const ssize_t n = ::send(fd_, p.data(), p.size(), MSG_NOSIGNAL);
    if (n > 0) {
      s_.out().consume(static_cast<std::size_t>(n));
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && errno == EAGAIN) {
      if (!wait) return;
      pollfd pf{fd_, POLLOUT, 0};
      ::poll(&pf, 1, 100);
      continue;
    }
    close();  // the connection is gone
    return;
  }
}

bool ClientConn::send(ouch::Bytes payload) {
  if (fd_ < 0) return false;
  const std::uint64_t now = monotonic_ns();
  if (!s_.send(payload, now)) {
    if (s_.state() != soup::ClientState::LoggedIn) return false;
    flush(true);  // full send buffer: make room, then retry once
    if (!s_.send(payload, now)) return false;
  }
  ++sent_;
  flush(false);
  last_send_tsc_ = pipeline::ticks();
  return true;
}

long ClientConn::read_some(int timeout_ms) {
  pollfd pf{fd_, POLLIN, 0};
  const int r = ::poll(&pf, 1, timeout_ms);
  if (r <= 0) return 0;
  const ssize_t n = ::recv(fd_, rbuf_.data(), rbuf_.size(), 0);
  if (n == 0) return -1;
  if (n < 0) return (errno == EAGAIN || errno == EINTR) ? 0 : -1;
  return static_cast<long>(n);
}

namespace {

void post(EpollServer& srv, const Control& c) {
  while (!srv.post(c)) std::this_thread::yield();
}

/// Waits for the reply to barrier `id`, reading the connection meanwhile.
template <class Deliver>
bool wait_reply(EpollServer& srv, ClientConn& conn, std::uint64_t id, int timeout_ms, ControlReply& out,
                Deliver&& deliver) {
  const std::uint64_t deadline = monotonic_ns() + ms_to_ns(timeout_ms);
  for (;;) {
    if (srv.poll_reply(out)) return out.id == id;
    (void)conn.poll(0, deliver);
    pollfd pf{srv.reply_fd(), POLLIN, 0};
    if (::poll(&pf, 1, 1) > 0) {
      std::uint64_t n = 0;
      [[maybe_unused]] const ssize_t r = ::read(srv.reply_fd(), &n, sizeof n);
    }
    if (monotonic_ns() > deadline) return false;
  }
}

/// Reads until the client's next expected sequence number is `next`.
template <class Deliver>
bool read_to(ClientConn& conn, std::uint64_t next, int timeout_ms, Deliver&& deliver) {
  const std::uint64_t deadline = monotonic_ns() + ms_to_ns(timeout_ms);
  while (conn.session().next_sequence() < next) {
    if (!conn.poll(1, deliver) || monotonic_ns() > deadline) return false;
  }
  return true;
}

}  // namespace

// --- LockstepExchange ------------------------------------------------------------------

void LockstepExchange::begin_input(const md::FeedInput& in, std::span<const md::FeedEvent> events) {
  base_ = events.data();
  Control c;
  c.kind = Control::Kind::Input;
  c.in = in;
  post(*srv_, c);
}

void LockstepExchange::quiesce(sim::Reports& out) {
  if (failed_) return;
  ++st_.barriers;
  Control b;
  b.kind = Control::Kind::Barrier;
  b.account = account_;
  b.inbound = conn_->sent();
  b.id = next_barrier_++;
  post(*srv_, b);
  auto deliver = [&](std::uint64_t, ouch::Bytes p) {
    ++st_.messages;
    gw_->on_message(p, out);
  };
  ControlReply r;
  if (!wait_reply(*srv_, *conn_, b.id, timeout_ms_, r, deliver) || !read_to(*conn_, r.next_sequence, timeout_ms_, deliver)) {
    failed_ = true;
  }
}

void LockstepExchange::submit(const strategy::OrderRequest& r, const md::Book&, sim::Reports& out) {
  gw_->submit(r, *conn_, out);
  quiesce(out);
}

void LockstepExchange::on_market(const md::FeedEvent& e, sim::Reports& out) {
  if (e.kind != md::FeedEvent::Kind::BookUpdate) return;
  std::array<std::uint64_t, 1> any{};
  if (gw_->open_ids(any) == 0) {  // nothing of ours rests: nothing can fill
    ++st_.events_skipped;
    return;
  }
  ++st_.events_sent;
  Control c;
  c.kind = Control::Kind::Event;
  c.event = static_cast<std::uint32_t>(&e - base_);
  post(*srv_, c);
  quiesce(out);
}

// --- FreeRunExchange ----------------------------------------------------------------------

void FreeRunExchange::begin_input(const md::FeedInput& in, std::span<const md::FeedEvent>) {
  Control c;
  c.kind = Control::Kind::Input;
  c.in = in;
  post(*srv_, c);
}

void FreeRunExchange::account_reports(const sim::Reports& out, std::size_t from) {
  for (std::size_t i = from; i < out.size(); ++i) {
    const sim::Report& r = out[i];
    const auto q = static_cast<std::int64_t>(r.qty);
    (r.side == sim::Side::Buy ? open_buy_ : open_sell_) -= q;
    if (r.kind == sim::Report::Kind::Fill) {
      position_ += r.side == sim::Side::Buy ? q : -q;
      cash_ += (r.side == sim::Side::Buy ? -q : q) * static_cast<std::int64_t>(r.price);
    }
  }
}

void FreeRunExchange::submit(const strategy::OrderRequest& r, const md::Book&, sim::Reports& out) {
  const std::size_t from = out.size();
  if (r.kind == strategy::OrderRequest::Kind::New) {
    (r.side == sim::Side::Buy ? open_buy_ : open_sell_) += r.qty;
    peak_exposure_ = std::max({peak_exposure_, position_ + open_buy_, open_sell_ - position_});
  }
  gw_->submit(r, *conn_, out);
  account_reports(out, from);
}

std::uint32_t FreeRunExchange::deliver_available(int timeout_ms, sim::Reports& out) {
  std::uint32_t n = 0;
  (void)conn_->poll(timeout_ms, [&](std::uint64_t, ouch::Bytes p) {
    ++n;
    ++st_.messages;
    gw_->on_message(p, out);
  });
  return n;
}

void FreeRunExchange::reconnect(sim::Reports& out) {
  ++st_.reconnects;
  conn_->close();
  gw_->on_disconnected();
  if (journal_ != nullptr) journal_->push(ClientJournal::kDisconnect);
  const std::uint64_t deadline = monotonic_ns() + ms_to_ns(timeout_ms_);
  if (!conn_->connect(srv_->port())) {
    fail("connect");
    return;
  }
  auto record = [&](std::uint32_t n) {
    if (n > 0 && journal_ != nullptr) journal_->push(ClientJournal::kDeliver, n);
  };
  while (conn_->session().state() == soup::ClientState::LoginSent && monotonic_ns() < deadline) {
    record(deliver_available(1, out));
  }
  if (conn_->session().state() != soup::ClientState::LoggedIn) {
    fail("login");
    return;
  }
  gw_->on_logged_in(*conn_);
  // The first Server Heartbeat after the login: the server had nothing left to send.
  const std::uint64_t hb = conn_->heartbeats();
  while (conn_->heartbeats() == hb && monotonic_ns() < deadline) record(deliver_available(1, out));
  if (conn_->heartbeats() == hb) {
    fail("replay");
    return;
  }
  gw_->on_replay_complete(out);
  if (journal_ != nullptr) journal_->push(ClientJournal::kReplayComplete);
}

void FreeRunExchange::on_market(const md::FeedEvent& e, sim::Reports& out) {
  if (e.kind != md::FeedEvent::Kind::BookUpdate) return;
  const std::size_t from = out.size();
  ++calls_;
  if (journal_ != nullptr) journal_->push(ClientJournal::kCall);
  if (next_drop_ < drops_.size() && drops_[next_drop_] == calls_) {
    ++next_drop_;
    reconnect(out);
  }
  const std::uint32_t n = deliver_available(0, out);
  if (n > 0 && journal_ != nullptr) journal_->push(ClientJournal::kDeliver, n);
  account_reports(out, from);
}

void FreeRunExchange::drain(sim::Reports& out) {
  const std::size_t from = out.size();
  if (journal_ != nullptr) journal_->push(ClientJournal::kDrain);
  std::array<std::uint64_t, 256> ids{};
  const std::size_t k = gw_->open_ids(ids);
  for (std::size_t i = 0; i < k; ++i) {
    gw_->submit(strategy::OrderRequest{strategy::OrderRequest::Kind::Cancel, ids[i], sim::Side::Buy, 0, 0}, *conn_, out);
  }
  Control b;
  b.kind = Control::Kind::Barrier;
  b.account = account_;
  b.inbound = conn_->sent();
  b.id = next_barrier_++;
  post(*srv_, b);
  std::uint32_t n = 0;
  auto deliver = [&](std::uint64_t, ouch::Bytes p) {
    ++n;
    gw_->on_message(p, out);
  };
  ControlReply r;
  if (!wait_reply(*srv_, *conn_, b.id, timeout_ms_, r, deliver)) {
    fail("drain: barrier");
  } else if (!read_to(*conn_, r.next_sequence, timeout_ms_, deliver)) {
    fail("drain: read");
  }
  if (n > 0 && journal_ != nullptr) journal_->push(ClientJournal::kDeliver, n);
  account_reports(out, from);
}

// --- ReplayExchange ---------------------------------------------------------------------------

void ReplayExchange::submit(const strategy::OrderRequest& r, const md::Book&, sim::Reports& out) {
  gw_->submit(r, null_, out);
}

void ReplayExchange::on_market(const md::FeedEvent& e, sim::Reports& out) {
  if (e.kind != md::FeedEvent::Kind::BookUpdate) return;
  const auto& ops = j_->ops;
  if (op_ >= ops.size() || (ops[op_] >> 28U) != ClientJournal::kCall) {
    failed_ = true;  // the replay diverged from the recording
    return;
  }
  ++op_;
  while (op_ < ops.size()) {
    const std::uint32_t op = ops[op_] >> 28U;
    const std::uint32_t v = ops[op_] & 0x0fffffffU;
    if (op == ClientJournal::kCall || op == ClientJournal::kDrain) break;
    ++op_;
    if (op == ClientJournal::kDeliver) {
      for (std::uint32_t i = 0; i < v; ++i) {
        if (next_msg_ >= log_->next_sequence()) {
          failed_ = true;
          return;
        }
        gw_->on_message(log_->at(next_msg_++), out);
      }
    } else if (op == ClientJournal::kDisconnect) {
      gw_->on_disconnected();
    } else if (op == ClientJournal::kReplayComplete) {
      gw_->on_replay_complete(out);
    }
  }
}

bool ReplayExchange::complete() const noexcept {
  return !failed_ && (op_ == j_->ops.size() || (j_->ops[op_] >> 28U) == ClientJournal::kDrain);
}

}  // namespace lob::net
