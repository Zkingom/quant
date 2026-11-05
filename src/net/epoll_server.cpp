#include "lob/net/epoll_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <ctime>
#include <system_error>

namespace lob::net {

namespace {

constexpr std::uint64_t kTagListen = 1;
constexpr std::uint64_t kTagControl = 2;

[[noreturn]] void fail(const char* what) { throw std::system_error(errno, std::generic_category(), what); }

void signal_fd(int fd) noexcept {
  const std::uint64_t one = 1;
  [[maybe_unused]] const ssize_t r = ::write(fd, &one, sizeof one);  // a full counter still wakes the reader
}

}  // namespace

std::uint64_t monotonic_ns() noexcept {
  timespec ts{};
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL + static_cast<std::uint64_t>(ts.tv_nsec);
}

EpollServer::EpollServer(OuchExchange& ex, const ServerOptions& opt)
    : ex_(ex),
      opt_(opt),
      control_(opt.control_capacity),
      replies_(opt.control_capacity),
      processed_(ex.accounts(), 0),
      rbuf_(1U << 16),
      reject_log_(soup::field<10>(""), 1, 1) {
  barriers_.reserve(opt.control_capacity);
  epfd_ = ::epoll_create1(EPOLL_CLOEXEC);
  if (epfd_ < 0) fail("epoll_create1");
  control_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  reply_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (control_fd_ < 0 || reply_fd_ < 0) fail("eventfd");
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (listen_fd_ < 0) fail("socket");
  const int yes = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(opt.port);
  if (::inet_pton(AF_INET, opt.address.c_str(), &addr.sin_addr) != 1) fail("inet_pton");
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) fail("bind");
  if (::listen(listen_fd_, 64) != 0) fail("listen");
  socklen_t len = sizeof addr;
  if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) fail("getsockname");
  port_ = ntohs(addr.sin_port);
  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.u64 = kTagListen;
  if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, listen_fd_, &ev) != 0) fail("epoll_ctl listen");
  ev.data.u64 = kTagControl;
  if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, control_fd_, &ev) != 0) fail("epoll_ctl control");
}

EpollServer::~EpollServer() {
  for (auto& c : conns_) {
    if (c->fd >= 0) ::close(c->fd);
  }
  for (const int fd : {listen_fd_, control_fd_, reply_fd_, epfd_}) {
    if (fd >= 0) ::close(fd);
  }
}

bool EpollServer::post(const Control& c) {
  if (!control_.try_push(c)) return false;
  signal_fd(control_fd_);
  return true;
}

std::size_t EpollServer::logged_in() const noexcept {
  std::size_t n = 0;
  for (const auto& c : conns_) {
    if (c->fd >= 0 && c->session && c->session->state() == soup::ServerState::LoggedIn) ++n;
  }
  return n;
}

void EpollServer::run() {
  const int wait = opt_.busy_poll ? 0 : 1000;
  while (run_once(wait)) {
  }
}

std::uint64_t EpollServer::next_deadline() const noexcept {
  std::uint64_t d = UINT64_MAX;
  for (const auto& c : conns_) {
    if (c->fd < 0) continue;
    d = std::min(d, c->session ? c->session->next_deadline() : c->opened + opt_.timers.login_timeout_ns);
  }
  return d;
}

bool EpollServer::run_once(int max_wait_ms) {
  if (stopped_) return false;
  std::uint64_t now = monotonic_ns();
  const std::uint64_t deadline = next_deadline();
  int wait = max_wait_ms;
  if (deadline != UINT64_MAX) {
    const std::uint64_t ms = deadline > now ? (deadline - now + 999'999) / 1'000'000 : 0;
    wait = static_cast<int>(std::min<std::uint64_t>(ms, static_cast<std::uint64_t>(max_wait_ms)));
  }
  epoll_event events[64];
  const int n = ::epoll_wait(epfd_, events, 64, wait);
  if (n < 0 && errno != EINTR) fail("epoll_wait");
  now = monotonic_ns();
  for (int i = 0; i < n; ++i) {
    const epoll_event& e = events[i];
    if (e.data.u64 == kTagListen) {
      accept_all(now);
    } else if (e.data.u64 == kTagControl) {
      std::uint64_t count = 0;
      [[maybe_unused]] const ssize_t r = ::read(control_fd_, &count, sizeof count);
      drain_control();
    } else {
      Conn& c = *static_cast<Conn*>(e.data.ptr);
      if (c.fd < 0) continue;
      if ((e.events & (EPOLLIN | EPOLLHUP | EPOLLERR)) != 0) on_readable(c, now);
      if (c.fd >= 0 && (e.events & EPOLLOUT) != 0) flush(c, now);
    }
  }
  drain_control();  // also when the eventfd wake-up is still pending
  pump_all(monotonic_ns());
  answer_barriers();
  sweep_closed();
  return !stopped_;
}

void EpollServer::accept_all(std::uint64_t now) {
  for (;;) {
    const int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) return;  // EAGAIN (or a transient error): nothing more to accept now
    const int yes = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof yes);
    auto c = std::make_unique<Conn>();
    c->fd = fd;
    c->opened = now;
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.ptr = c.get();
    if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) != 0) {
      ::close(fd);
      continue;
    }
    ++st_.accepted;
    conns_.push_back(std::move(c));
  }
}

void EpollServer::on_readable(Conn& c, std::uint64_t now) {
  const ssize_t n = ::recv(c.fd, rbuf_.data(), rbuf_.size(), 0);
  if (n == 0) {
    if (c.session) c.session->on_disconnected();
    close_conn(c);
    return;
  }
  if (n < 0) {
    if (errno == EAGAIN || errno == EINTR) return;
    if (c.session) c.session->on_disconnected();
    close_conn(c);
    return;
  }
  st_.bytes_in += static_cast<std::uint64_t>(n);
  feed(c, {rbuf_.data(), static_cast<std::size_t>(n)}, now);
}

void EpollServer::bind_account(Conn& c, std::uint64_t now) {
  // Skip Debug packets; bind at the first complete other packet.
  std::size_t i = 0;
  while (i + 2 <= c.pre.size()) {
    const std::size_t len = (static_cast<std::size_t>(c.pre[i]) << 8U) | c.pre[i + 1];
    if (len == 0 || len > soup::kMaxDebugPacket) break;  // the session reports the framing error
    if (i + 2 + len > c.pre.size()) return;              // incomplete: wait
    if (c.pre[i + 2] == static_cast<std::uint8_t>(soup::type::kDebug)) {
      i += 2 + len;
      continue;
    }
    soup::LoginRequest req;
    if (c.pre[i + 2] == static_cast<std::uint8_t>(soup::type::kLoginRequest) &&
        soup::parse_login_request({c.pre.data() + i + 3, len - 1}, req)) {
      c.account = ex_.find_account({req.user.data(), req.user.size()});
    }
    break;
  }
  if (i + 2 > c.pre.size()) return;
  soup::ServerConfig sc;
  sc.timers = opt_.timers;
  sc.out_capacity = opt_.out_capacity;
  if (c.account != OuchExchange::npos) {
    const AccountConfig& a = ex_.account(c.account);
    sc.user = a.user;
    sc.password = a.password;
    c.session = std::make_unique<soup::ServerConnection>(sc, ex_.log(c.account), c.opened);
  } else {
    sc.user = "\x01";  // matches no Login Request: unknown users get Login Rejected 'A'
    sc.password = "\x01";
    c.session = std::make_unique<soup::ServerConnection>(sc, reject_log_, c.opened);
  }
  (void)now;
}

void EpollServer::feed(Conn& c, soup::Bytes in, std::uint64_t now) {
  if (!c.session) {
    c.pre.insert(c.pre.end(), in.begin(), in.end());
    bind_account(c, now);
    if (!c.session) {
      if (c.pre.size() > 2 + soup::kMaxDebugPacket) close_conn(c);
      return;
    }
    const std::vector<std::uint8_t> pre = std::move(c.pre);
    c.pre.clear();
    feed(c, pre, now);
    return;
  }
  const soup::ServerState before = c.session->state();
  c.session->on_bytes(in, now, [&](soup::Bytes p) {
    ++st_.inbound;
    if (hook_ != nullptr) hook_(hook_ctx_, c.account, p);
    if (journal_ != nullptr) {
      JournalEntry e;
      e.kind = JournalEntry::Kind::Message;
      e.account = static_cast<std::uint32_t>(c.account);
      e.now = now;
      e.len = static_cast<std::uint16_t>(std::min(p.size(), e.bytes.size()));
      std::copy_n(p.begin(), e.len, e.bytes.begin());
      journal_->entries.push_back(e);
    }
    ex_.on_message(c.account, p, now);
    ++processed_[c.account];
  });
  if (before == soup::ServerState::AwaitLogin) {
    if (c.session->state() == soup::ServerState::LoggedIn) {
      ++st_.logins;
      processed_[c.account] = 0;  // barriers count inbound messages per connection
      // Takeover: a successful login closes the account's older connection
      // (the client reconnected before the server saw the old one drop).
      // Its cancel-on-disconnect messages go to the log, so the new
      // connection replays them.
      for (auto& o : conns_) {
        if (o.get() != &c && o->fd >= 0 && o->account == c.account && o->session &&
            o->session->state() == soup::ServerState::LoggedIn) {
          o->session->on_disconnected();
          close_conn(*o);
        }
      }
    }
    if (c.session->close_reason() == soup::CloseReason::LoginRejected) ++st_.login_rejects;
  }
}

void EpollServer::drain_control() {
  Control c;
  while (control_.try_pop(c)) {
    switch (c.kind) {
      case Control::Kind::Input: {
        if (journal_ != nullptr) {
          JournalEntry e;
          e.kind = JournalEntry::Kind::Input;
          e.in = c.in;
          journal_->entries.push_back(e);
        }
        const std::size_t n = ex_.apply_input(c.in);
        if (opt_.auto_market_events) {
          for (std::size_t k = 0; k < n; ++k) ex_.market_event(k);
        }
        break;
      }
      case Control::Kind::Event:
        if (journal_ != nullptr) {
          JournalEntry e;
          e.kind = JournalEntry::Kind::Event;
          e.event = c.event;
          journal_->entries.push_back(e);
        }
        ex_.market_event(c.event);
        break;
      case Control::Kind::Barrier:
        ++st_.barriers;
        barriers_.push_back(c);
        break;
      case Control::Kind::EndOfDay:
        if (journal_ != nullptr) {
          JournalEntry e;
          e.kind = JournalEntry::Kind::EndOfDay;
          journal_->entries.push_back(e);
        }
        ex_.end_of_day();
        break;
      case Control::Kind::Stop:
        stopped_ = true;
        break;
    }
  }
}

void EpollServer::answer_barriers() {
  std::size_t kept = 0;
  bool any = false;
  for (const Control& b : barriers_) {
    if (b.account < processed_.size() && processed_[b.account] >= b.inbound) {
      // Cannot fail: the reply queue is as large as the control queue.
      (void)replies_.try_push(ControlReply{b.id, ex_.log(b.account).next_sequence()});
      any = true;
    } else {
      barriers_[kept++] = b;
    }
  }
  barriers_.resize(kept);
  if (any) signal_fd(reply_fd_);
}

void EpollServer::pump_all(std::uint64_t now) {
  for (auto& cp : conns_) {
    Conn& c = *cp;
    if (c.fd < 0) continue;
    if (!c.session) {
      if (now >= c.opened + opt_.timers.login_timeout_ns) close_conn(c);
      continue;
    }
    c.session->on_timer(now);  // timeouts, heartbeat, pump from the log
    flush(c, now);
    if (c.fd >= 0 && c.session->wants_close()) close_conn(c);  // after one attempt to flush
  }
}

void EpollServer::flush(Conn& c, std::uint64_t now) {
  while (c.fd >= 0 && c.session) {
    c.session->pump(now);
    const soup::Bytes p = c.session->pending();
    if (p.empty()) break;
    const ssize_t n = ::send(c.fd, p.data(), p.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n < 0) {
      if (errno == EAGAIN) {
        ++st_.would_block;
        if (!c.want_out) {
          epoll_event ev{};
          ev.events = EPOLLIN | EPOLLOUT;
          ev.data.ptr = &c;
          ::epoll_ctl(epfd_, EPOLL_CTL_MOD, c.fd, &ev);
          c.want_out = true;
        }
        return;
      }
      if (errno == EINTR) continue;
      c.session->on_disconnected();
      close_conn(c);
      return;
    }
    st_.bytes_out += static_cast<std::uint64_t>(n);
    c.session->consume(static_cast<std::size_t>(n), now);
  }
  if (c.fd >= 0 && c.want_out) {
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.ptr = &c;
    ::epoll_ctl(epfd_, EPOLL_CTL_MOD, c.fd, &ev);
    c.want_out = false;
  }
}

void EpollServer::close_conn(Conn& c) {
  if (c.fd < 0) return;
  // next_to_send() is set by a successful login and never reset.
  const bool was_logged_in = c.session && c.account != OuchExchange::npos && c.session->next_to_send() != 0;
  ::epoll_ctl(epfd_, EPOLL_CTL_DEL, c.fd, nullptr);
  ::close(c.fd);
  c.fd = -1;
  ++st_.closed;
  if (was_logged_in) {
    if (journal_ != nullptr) {
      JournalEntry e;
      e.kind = JournalEntry::Kind::Disconnect;
      e.account = static_cast<std::uint32_t>(c.account);
      journal_->entries.push_back(e);
    }
    ex_.on_disconnect(c.account);  // cancel-on-disconnect (if configured)
  }
}

void replay(const ServerJournal& j, OuchExchange& ex, bool auto_market_events) {
  for (const JournalEntry& e : j.entries) {
    switch (e.kind) {
      case JournalEntry::Kind::Input: {
        const std::size_t n = ex.apply_input(e.in);
        if (auto_market_events) {
          for (std::size_t k = 0; k < n; ++k) ex.market_event(k);
        }
        break;
      }
      case JournalEntry::Kind::Event:
        ex.market_event(e.event);
        break;
      case JournalEntry::Kind::Message:
        ex.on_message(e.account, {e.bytes.data(), e.len}, e.now);
        break;
      case JournalEntry::Kind::Disconnect:
        ex.on_disconnect(e.account);
        break;
      case JournalEntry::Kind::EndOfDay:
        ex.end_of_day();
        break;
    }
  }
}

void EpollServer::sweep_closed() {
  conns_.erase(std::remove_if(conns_.begin(), conns_.end(), [](const auto& c) { return c->fd < 0; }), conns_.end());
}

}  // namespace lob::net
