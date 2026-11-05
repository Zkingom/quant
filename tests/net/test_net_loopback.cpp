// L4.3: the epoll exchange server over real loopback TCP. The server runs on
// its own thread (labelled "concurrency": also run under TSan); the client
// side is the sans-IO soup::ClientSession driven by a small poll() loop.
// Every wait has a timeout, so a bug fails the test instead of hanging CI.

#include "lob/net/epoll_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

#include "lob/net/ouch_exchange.hpp"
#include "lob/soup/client.hpp"
#include "lob/testkit/itch_builder.hpp"
#include "lob/testkit/itch_samples.hpp"

namespace {

namespace md = lob::md;
namespace net = lob::net;
namespace ouch = lob::ouch;
namespace soup = lob::soup;
namespace tk = lob::testkit::itch;
namespace samples = lob::testkit::itch_samples;
using samples::kLocate;

net::ExchangeConfig exchange_config() {
  net::ExchangeConfig c;
  c.feed = samples::config();
  c.accounts = {net::AccountConfig{"A1", "pw", "FIRM", true, 0, 1}};
  c.log_messages = 10000;
  c.log_bytes = 1U << 20;
  c.max_orders = 256;
  return c;
}

/// Exchange + server thread. Market inputs go through the control queue.
struct Server {
  net::OuchExchange ex{exchange_config()};
  net::EpollServer srv;
  std::thread th;
  std::uint64_t seq{0};

  explicit Server(net::ServerOptions opt = {}) : srv(ex, opt) {
    th = std::thread([this] { srv.run(); });
    for (const auto& m : {tk::system_event('O', 1), tk::stock_directory(kLocate, "ACME", 3),
                          tk::trading_action(kLocate, "ACME", 'T', 4), tk::system_event('Q', 5),
                          tk::add_order(kLocate, 1, 'B', 100, "ACME", 10000, 10)}) {
      market(m);
    }
  }
  ~Server() { stop(); }
  void stop() {
    if (th.joinable()) {
      net::Control c;
      c.kind = net::Control::Kind::Stop;
      while (!srv.post(c)) std::this_thread::yield();
      th.join();
    }
  }
  void market(const tk::Buf& payload) {
    net::Control c;
    c.kind = net::Control::Kind::Input;
    c.in.seq = ++seq;
    c.in.kind = md::InputKind::Decoded;
    EXPECT_EQ(md::itch::decode(payload, c.in.msg), md::itch::DecodeStatus::Decoded);
    while (!srv.post(c)) std::this_thread::yield();
  }
  void control(net::Control c) {
    while (!srv.post(c)) std::this_thread::yield();
  }
  /// Waits for one barrier reply (timeout -> failure).
  net::ControlReply reply(int timeout_ms = 5000) {
    net::ControlReply r;
    for (int waited = 0; waited < timeout_ms; ++waited) {
      if (srv.poll_reply(r)) return r;
      pollfd p{srv.reply_fd(), POLLIN, 0};
      ::poll(&p, 1, 1);
      std::uint64_t n = 0;
      [[maybe_unused]] const ssize_t got = ::read(srv.reply_fd(), &n, sizeof n);
    }
    ADD_FAILURE() << "no barrier reply";
    return r;
  }
};

/// A SoupBinTCP client over a real socket.
struct Client {
  soup::ClientSession s;
  int fd{-1};
  std::vector<std::vector<std::uint8_t>> got;  // sequenced messages (OUCH outbound payloads)
  bool peer_closed{false};

  explicit Client(std::string user = "A1", std::string pw = "pw")
      : s([&] {
          soup::ClientConfig c;
          c.user = std::move(user);
          c.password = std::move(pw);
          return c;
        }()) {}
  ~Client() { close(); }

  void connect(std::uint16_t port) {
    fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT_GE(fd, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    ASSERT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a), 0) << errno;
    const int yes = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof yes);
    peer_closed = false;
    s.on_connected(net::monotonic_ns());
    flush();
  }
  void close() {
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
      s.on_disconnected();
    }
  }
  void flush() {
    while (fd >= 0 && !s.out().empty()) {
      const auto p = s.out().pending();
      const ssize_t n = ::send(fd, p.data(), p.size(), MSG_NOSIGNAL);
      if (n <= 0) return;
      s.out().consume(static_cast<std::size_t>(n));
    }
  }
  template <class M>
  void send(const M& m) {
    std::array<std::uint8_t, ouch::kMaxMessageSize> b{};
    const std::size_t n = ouch::encode(m, b);
    ASSERT_TRUE(until([&] { return s.state() == soup::ClientState::LoggedIn; }));  // data only after login
    ASSERT_TRUE(s.send({b.data(), n}, net::monotonic_ns()));
    flush();
  }
  /// Reads until pred() holds (true) or timeout (false).
  bool until(const std::function<bool()>& pred, int timeout_ms = 5000) {
    std::vector<std::uint8_t> buf(1U << 16);
    for (int i = 0; i < timeout_ms && !pred(); ++i) {
      if (fd < 0) return pred();
      pollfd p{fd, POLLIN, 0};
      if (::poll(&p, 1, 1) > 0) {
        const ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
        if (n <= 0) {
          peer_closed = true;
          close();
          return pred();
        }
        s.on_bytes({buf.data(), static_cast<std::size_t>(n)}, net::monotonic_ns(),
                   [&](std::uint64_t, soup::Bytes msg) { got.emplace_back(msg.begin(), msg.end()); });
      }
      s.on_timer(net::monotonic_ns());
      flush();
      if (s.wants_close()) close();
    }
    return pred();
  }
  template <class M>
  [[nodiscard]] std::size_t count() const {
    std::size_t n = 0;
    for (const auto& g : got) {
      ouch::Outbound m;
      if (ouch::decode_outbound(g, m) == ouch::DecodeStatus::Decoded && std::holds_alternative<M>(m)) ++n;
    }
    return n;
  }
};

ouch::EnterOrder enter(std::string_view token, std::uint32_t shares, std::uint32_t price) {
  ouch::EnterOrder e;
  e.token = ouch::make_token(token);
  e.side = ouch::Side::Buy;
  e.shares = shares;
  e.stock = ouch::make_symbol("ACME");
  e.price = price;
  e.time_in_force = ouch::kTifSystemHours;
  e.firm = ouch::alpha<4>("");
  return e;
}

TEST(NetLoopback, LoginEnterAndAcceptedOverRealSockets) {
  Server srv;
  Client c;
  c.connect(srv.srv.port());
  ASSERT_TRUE(c.until([&] { return c.s.state() == soup::ClientState::LoggedIn; }));
  ASSERT_TRUE(c.until([&] { return c.got.size() >= 1; }));  // Start of Day
  c.send(enter("L1", 100, 9900));
  ASSERT_TRUE(c.until([&] { return c.count<ouch::Accepted>() == 1; }));
  EXPECT_EQ(c.count<ouch::SystemEvent>(), 1U);
}

TEST(NetLoopback, UnknownUserIsRejectedNotAuthorized) {
  Server srv;
  Client c("NOBODY", "x");
  c.connect(srv.srv.port());
  ASSERT_TRUE(c.until([&] { return c.s.state() == soup::ClientState::Rejected; }));
  EXPECT_EQ(c.s.close_reason(), soup::CloseReason::LoginRejected);
}

TEST(NetLoopback, MarketInputThroughTheControlQueueProducesExecuted) {
  Server srv;
  Client c;
  c.connect(srv.srv.port());
  c.send(enter("M1", 100, 10000));
  ASSERT_TRUE(c.until([&] { return c.count<ouch::Accepted>() == 1; }));
  srv.market(tk::order_executed(kLocate, 1, 40, 5, 20));
  ASSERT_TRUE(c.until([&] { return c.count<ouch::Executed>() == 1; }));
}

TEST(NetLoopback, DisconnectCancelsOrdersAndReconnectReplaysTheCancels) {
  Server srv;
  Client c;
  c.connect(srv.srv.port());
  c.send(enter("D1", 100, 9900));
  c.send(enter("D2", 100, 9800));
  ASSERT_TRUE(c.until([&] { return c.count<ouch::Accepted>() == 2; }));
  const std::uint64_t next = c.s.next_sequence();
  c.close();  // connection lost
  c.connect(srv.srv.port());  // same session, next expected sequence number
  ASSERT_TRUE(c.until([&] { return c.count<ouch::Canceled>() == 2; }));
  EXPECT_EQ(c.s.logins(), 2U);
  EXPECT_EQ(c.s.next_sequence(), next + 2);
  for (const auto& g : c.got) {
    ouch::Outbound m;
    ASSERT_EQ(ouch::decode_outbound(g, m), ouch::DecodeStatus::Decoded);
    if (const auto* x = std::get_if<ouch::Canceled>(&m)) {
      EXPECT_EQ(x->reason, ouch::reason::kSystemCancel);
    }
  }
}

TEST(NetLoopback, SecondLoginOfTheSameAccountTakesOver) {
  Server srv;
  Client first;
  first.connect(srv.srv.port());
  ASSERT_TRUE(first.until([&] { return first.s.state() == soup::ClientState::LoggedIn; }));
  Client second;
  second.connect(srv.srv.port());
  ASSERT_TRUE(second.until([&] { return second.s.state() == soup::ClientState::LoggedIn; }));
  ASSERT_TRUE(first.until([&] { return first.peer_closed; }));  // the old connection is closed by the server
}

TEST(NetLoopback, BarrierIsAnsweredAfterTheInboundMessageWithTheWatermark) {
  Server srv;
  Client c;
  c.connect(srv.srv.port());
  ASSERT_TRUE(c.until([&] { return c.s.state() == soup::ClientState::LoggedIn; }));
  c.send(enter("B1", 100, 9900));
  net::Control b;
  b.kind = net::Control::Kind::Barrier;
  b.account = 0;
  b.inbound = 1;
  b.id = 42;
  srv.control(b);
  const net::ControlReply r = srv.reply();
  EXPECT_EQ(r.id, 42U);
  EXPECT_EQ(r.next_sequence, 3U);  // Start of Day (1) + Accepted (2)
  ASSERT_TRUE(c.until([&] { return c.s.next_sequence() == r.next_sequence; }));
  EXPECT_EQ(c.count<ouch::Accepted>(), 1U);
}

TEST(NetLoopback, EndOfDayEndsTheSession) {
  Server srv;
  Client c;
  c.connect(srv.srv.port());
  ASSERT_TRUE(c.until([&] { return c.s.state() == soup::ClientState::LoggedIn; }));
  net::Control e;
  e.kind = net::Control::Kind::EndOfDay;
  srv.control(e);
  ASSERT_TRUE(c.until([&] { return c.s.state() == soup::ClientState::SessionEnded; }));
  EXPECT_EQ(c.count<ouch::SystemEvent>(), 2U);  // S then E, then End of Session
}

}  // namespace
