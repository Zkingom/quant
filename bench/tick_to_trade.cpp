// lob_t2t: tick-to-trade measurement (L4.6). docs/design/L4_DESIGN.md §3.
//
//   lob_t2t [--messages N] [--seed S] [--pipelined] [--quick]
//           [--input PATH --symbol SYM]       real ITCH (local only; never committed)
//           [--client-cpu C] [--server-cpu C] pin the threads
//           [--busy-poll]                     the exchange thread spins instead of sleeping
//
// t0 = the TSC reading when framing / decoding of the ITCH frame began (the
// decoder thread in --pipelined mode). Only frames that produced an order
// are samples. Segments, each reported as p50 / p99 / p99.9 / max with one
// clock-read overhead subtracted:
//
//   inproc    T1 tick -> decision        risk approved, the gateway is called
//             T2 tick -> encoded         OUCH + SoupBinTCP bytes in the send buffer
//                                        (no syscall; the exchange core runs on the
//                                        same thread but after the frame, outside
//                                        the measured window)
//             T2 for the first message of the frame (any type: a re-quote
//                                        usually starts with a cancel)
//   loopback  T1                         as above, with a real connection
//             T3 tick -> trade           send() on the loopback TCP socket returned,
//                                        for new orders and for the first message
//                                        of the frame (a frame can send several:
//                                        a new order often follows a cancel, whose
//                                        send() is then inside its window)
// Replies are read, and the market input is forwarded to the exchange
// thread, AFTER the frame's orders are out (send first, then everything
// else); replies reach the strategy at the next market event. No syscall
// other than the orders' own send() sits inside a measured window.
//             T4 wire -> exchange        client before send() -> the exchange
//                                        thread has the decoded Unsequenced Data
//                                        (two threads, one TSC: see clock.hpp)
//   ack       T5 tick -> ack             the Accepted / Rejected is decoded on the
//                                        client (the client spins for it after
//                                        each new order: a round trip, not the
//                                        free-run delivery delay)
//
// Wall-clock numbers depend on the machine: the tool prints the environment
// (CPU, cores, kernel, hypervisor flag, compiler, build type). In-process
// numbers say what the software path costs; loopback numbers add the kernel
// TCP stack and the wake-up of the exchange thread; neither is a wire-to-
// wire number (no NIC, no switch, no exchange gateway; L4 design §3.2-3.3).

#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "lob/gateway/gateway.hpp"
#include "lob/net/epoll_server.hpp"
#include "lob/net/lockstep.hpp"
#include "lob/net/ouch_exchange.hpp"
#include "lob/pipeline/clock.hpp"
#include "lob/pipeline/pipeline.hpp"
#include "lob/sim/trading.hpp"
#include "lob/soup/client.hpp"
#include "lob/testkit/itch_builder.hpp"
#include "lob/testkit/itch_generator.hpp"

namespace {

namespace gw = lob::gateway;
namespace md = lob::md;
namespace net = lob::net;
namespace ouch = lob::ouch;
namespace pl = lob::pipeline;
namespace sim = lob::sim;
namespace soup = lob::soup;
namespace st = lob::strategy;
namespace tk = lob::testkit::itch;

struct Opts {
  std::size_t messages{200'000};
  std::uint64_t seed{1};
  bool pipelined{false};
  std::string input;
  std::string symbol;
  int client_cpu{-1};
  int server_cpu{-1};
  bool busy_poll{false};
};

void pin(int cpu) {
  if (cpu < 0) return;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<unsigned>(cpu), &set);
  pthread_setaffinity_np(pthread_self(), sizeof set, &set);
}

/// Fixed-capacity sample buffer (no allocation while measuring).
struct Samples {
  std::vector<std::uint64_t> v;
  std::size_t n{0};
  explicit Samples(std::size_t cap = 0) : v(cap) {}
  void add(std::uint64_t t) noexcept {
    if (n < v.size()) v[n++] = t;
  }
};

struct Stats {
  std::size_t n{0};
  double p50{0}, p99{0}, p999{0}, max{0};
};

Stats summarize(Samples& s, const pl::ClockCalibration& c) {
  Stats r;
  r.n = s.n;
  if (s.n == 0) return r;
  std::sort(s.v.begin(), s.v.begin() + static_cast<std::ptrdiff_t>(s.n));
  auto at = [&](double q) {
    const auto i = std::min(s.n - 1, static_cast<std::size_t>(q * static_cast<double>(s.n)));
    return std::max(0.0, (static_cast<double>(s.v[i]) - c.overhead_ticks) * c.ns_per_tick);
  };
  r.p50 = at(0.50);
  r.p99 = at(0.99);
  r.p999 = at(0.999);
  r.max = std::max(0.0, (static_cast<double>(s.v[s.n - 1]) - c.overhead_ticks) * c.ns_per_tick);
  return r;
}

/// What one run measures.
struct Meter {
  const std::uint64_t* t0{nullptr};  // the BookStage's decode-start of the current frame
  std::uint64_t frame{0};            // input sequence of the current frame
  Samples t1, t2, t2_first, t3, t3_first, t5;
  std::uint64_t last_msg_frame{0};  // frame of the last message sent (first-of-frame detection)
  std::vector<std::uint64_t> send_start;  // by token counter (T4, client side)
  std::vector<std::uint64_t> arrive;      // by token counter (T4, written by the exchange thread)
  explicit Meter(std::size_t cap)
      : t1(cap), t2(cap), t2_first(cap), t3(cap), t3_first(cap), t5(cap), send_start(cap + 2, 0), arrive(cap + 2, 0) {}
  /// True once per frame: the first order-entry message of the frame.
  bool first_of_frame() noexcept {
    const bool first = last_msg_frame != frame;
    last_msg_frame = frame;
    return first;
  }
};

/// Token counter of one of our tokens (last 8 characters, base 36).
std::uint64_t counter_of(const std::uint8_t* tok) noexcept {
  std::uint64_t n = 0;
  for (int i = 6; i < 14; ++i) {
    const char c = static_cast<char>(tok[i]);
    n = n * 36 + static_cast<std::uint64_t>(c <= '9' ? c - '0' : c - 'A' + 10);
  }
  return n;
}

/// In-process transport: the SoupBinTCP client session (logged in, in
/// memory) frames the message -- T2 -- and the bytes are kept; the exchange
/// core gets them after the frame (end_input), outside the measured window.
struct MemTransport final : gw::Transport {
  soup::ClientSession session;
  net::OuchExchange* ex{nullptr};
  Meter* m{nullptr};
  soup::Deframer deframer;
  std::vector<std::uint8_t> queued;
  explicit MemTransport(const soup::ClientConfig& c) : session(c) {
    queued.reserve(1U << 20);
    session.on_connected(0);
    session.out().consume(session.out().pending().size());
    std::string acc(10, ' ');
    acc += std::string(19, ' ') + "1";
    std::vector<std::uint8_t> pkt{0, static_cast<std::uint8_t>(acc.size() + 1), 'A'};
    pkt.insert(pkt.end(), acc.begin(), acc.end());
    session.on_bytes(pkt, 0, [](std::uint64_t, soup::Bytes) {});
  }
  bool send(ouch::Bytes payload) override {
    if (!session.send(payload, 0)) return false;
    const std::uint64_t t = pl::ticks() - *m->t0;
    if (payload[0] == 'O') m->t2.add(t);
    if (m->first_of_frame()) m->t2_first.add(t);
    const auto bytes = session.out().pending();
    queued.insert(queued.end(), bytes.begin(), bytes.end());
    session.out().consume(bytes.size());
    return true;
  }
  /// After the frame: the exchange processes what was sent.
  void flush_to_exchange() {
    (void)deframer.feed(queued, [&](char t, soup::Bytes p) {
      if (t == soup::type::kUnsequencedData) ex->on_message(0, p, 0);
      return true;
    });
    queued.clear();
  }
};

/// Loopback transport: ClientConn (real socket); T3 at send() return, T4 start.
struct TimedConn final : gw::Transport {
  net::ClientConn* conn{nullptr};
  Meter* m{nullptr};
  bool send(ouch::Bytes payload) override {
    const bool order = payload[0] == 'O';
    if (order) m->send_start[std::min<std::uint64_t>(counter_of(payload.data() + 1), m->send_start.size() - 1)] = pl::ticks();
    const bool ok = conn->send(payload);
    if (ok) {
      const std::uint64_t t = conn->last_send_tsc() - *m->t0;
      if (order) m->t3.add(t);
      if (m->first_of_frame()) m->t3_first.add(t);
    }
    return ok;
  }
};

enum class Mode { InProc, Loopback, Ack };

/// TradingStage's exchange for the measurement (free-run delivery).
class MeterExchange {
 public:
  MeterExchange(Mode mode, gw::OrderGateway& g, gw::Transport& t, Meter& m, net::OuchExchange* ex, net::EpollServer* srv,
                net::ClientConn* conn)
      : mode_(mode), gw_(&g), tr_(&t), m_(&m), ex_(ex), srv_(srv), conn_(conn) {
    pending_.reserve(1U << 16);
  }

  void begin_input(const md::FeedInput& in, std::span<const md::FeedEvent>) { m_->frame = in.seq; }
  /// Send first, read afterwards: after the frame's orders are out, the
  /// in-process exchange processes them and the frame (in loopback mode it
  /// runs on its own thread), and the replies are read and decoded by the
  /// gateway. Their reports reach the strategy at the next market event, so
  /// no receive work sits inside the measured t0 -> send window.
  void end_input(const md::FeedInput& in) {
    if (mode_ == Mode::InProc) {
      mem_->flush_to_exchange();
      ex_->on_market(in);
    } else {
      // The market input goes to the exchange thread after our orders: the
      // post wakes it with an eventfd write (a syscall) that must not sit in
      // the measured window. The exchange's book then trails ours by one
      // frame -- irrelevant for the optimistic fill model used here.
      net::Control c;
      c.kind = net::Control::Kind::Input;
      c.in = in;
      while (!srv_->post(c)) std::this_thread::yield();
    }
    collect();
  }
  void submit(const st::OrderRequest& r, const md::Book&, sim::Reports& out) {
    if (r.kind == st::OrderRequest::Kind::New) m_->t1.add(pl::ticks() - *m_->t0);
    gw_->submit(r, *tr_, out);
    if (mode_ == Mode::Ack && r.kind == st::OrderRequest::Kind::New) {
      const gw::OrderView* o = gw_->find(r.client_id);
      for (int spins = 0; o != nullptr && o->state == gw::OrderState::PendingNew && spins < 10'000'000; ++spins) {
        (void)conn_->poll(0, [&](std::uint64_t, ouch::Bytes p) { gw_->on_message(p, out); });
      }
      if (o != nullptr && o->state != gw::OrderState::PendingNew) m_->t5.add(pl::ticks() - *m_->t0);
    }
  }
  void on_market(const md::FeedEvent& e, sim::Reports& out) {
    if (e.kind != md::FeedEvent::Kind::BookUpdate) return;
    std::size_t i = 0;
    for (; i < pending_.size() && out.size() < sim::Reports::kCapacity; ++i) out.push(pending_[i]);
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(i));
  }
  void set_mem(MemTransport* m) noexcept { mem_ = m; }
  std::size_t open_ids(std::span<std::uint64_t> out) const { return gw_->open_ids(out); }
  [[nodiscard]] bool inconsistent() const noexcept { return gw_->inconsistent(); }

 private:
  /// Reads every available reply into the gateway; its reports wait in pending_.
  void collect() {
    sim::Reports r;
    auto keep = [&] {
      for (std::size_t i = 0; i < r.size(); ++i) pending_.push_back(r[i]);
      r.clear();
    };
    if (mode_ == Mode::InProc) {
      for (; read_ < ex_->log(0).next_sequence(); ++read_) {
        gw_->on_message(ex_->log(0).at(read_), r);
        keep();
      }
      return;
    }
    (void)conn_->poll(0, [&](std::uint64_t, ouch::Bytes p) {
      gw_->on_message(p, r);
      keep();
    });
  }

  Mode mode_;
  gw::OrderGateway* gw_;
  gw::Transport* tr_;
  Meter* m_;
  net::OuchExchange* ex_;
  net::EpollServer* srv_;
  net::ClientConn* conn_;
  MemTransport* mem_{nullptr};
  std::vector<sim::Report> pending_;
  std::uint64_t read_{1};
};

/// Pipeline downstream: exposes the frame's decode-start tick to the meter.
template <class Stage>
struct Down {
  Stage& stage;
  void on_feed(const md::FeedInput& in, std::span<const md::FeedEvent> ev, const md::FeedHandler& f, std::uint64_t t,
               pl::LatencyRecorder* rec, bool s) {
    stage.exchange().begin_input(in, ev);
    stage.on_feed(in, ev, f, t, rec, s);
    stage.exchange().end_input(in);
  }
};

sim::TradingConfig trading_cfg() {
  sim::TradingConfig c;
  c.sim.model = sim::FillModel::Optimistic;
  c.risk.bucket_capacity = 1'000'000;
  c.risk.refill_per_second = 1'000'000'000;
  c.risk.max_rejects_in_window = 1'000'000'000;
  c.risk.max_position = 1'000'000;
  c.timer_interval_ns = 1'000'000;
  return c;
}

struct RunOut {
  Stats t1, t2, t2_first, t3, t3_first, t4, t5;
  std::uint64_t requests{0};
  std::uint64_t fills{0};
  bool ok{true};
};

void on_inbound(void* ctx, std::size_t, soup::Bytes p) {
  auto* m = static_cast<Meter*>(ctx);
  if (p.size() == ouch::kEnterOrderSize && p[0] == 'O') {
    m->arrive[std::min<std::uint64_t>(counter_of(p.data() + 1), m->arrive.size() - 1)] = pl::ticks();
  }
}

RunOut run_mode(Mode mode, const Opts& o, pl::ChunkSource& src, const md::FeedConfig& fc, const pl::ClockCalibration& clock) {
  Meter m(o.messages + 16);
  net::ExchangeConfig ec;
  ec.feed = fc;
  ec.accounts = {net::AccountConfig{"T2T", "pw"}};
  ec.log_messages = o.messages * 4 + 1024;
  ec.log_bytes = ec.log_messages * 48;
  ec.max_orders = o.messages * 2 + 1024;
  net::OuchExchange ex(ec);
  gw::GatewayConfig gc;
  gc.symbol = fc.symbol;
  gc.max_orders = ec.max_orders;
  gw::OrderGateway g(gc);
  soup::ClientConfig cc;
  cc.user = "T2T";
  cc.password = "pw";

  std::unique_ptr<net::EpollServer> srv;
  std::thread th;
  std::unique_ptr<net::ClientConn> conn;
  MemTransport mem(cc);
  TimedConn timed;
  mem.ex = &ex;
  mem.m = &m;
  if (mode != Mode::InProc) {
    net::ServerOptions so;
    so.auto_market_events = true;
    so.control_capacity = 1U << 16;
    so.busy_poll = o.busy_poll;
    srv = std::make_unique<net::EpollServer>(ex, so);
    srv->set_inbound_hook(on_inbound, &m);
    th = std::thread([&] {
      pin(o.server_cpu);
      srv->run();
    });
    conn = std::make_unique<net::ClientConn>(cc);
    if (!conn->connect(srv->port())) {
      std::fprintf(stderr, "connect failed\n");
      std::exit(1);
    }
    for (int i = 0; i < 5000 && conn->session().state() == soup::ClientState::LoginSent; ++i) {
      (void)conn->poll(1, [&](std::uint64_t, ouch::Bytes p) {
        sim::Reports r;
        g.on_message(p, r);
      });
    }
    timed.conn = conn.get();
    timed.m = &m;
  }
  gw::Transport& tr = mode == Mode::InProc ? static_cast<gw::Transport&>(mem) : static_cast<gw::Transport&>(timed);
  st::MarketMaker mm{};
  sim::TradingStage<st::MarketMaker, MeterExchange> t{trading_cfg(), mm,
                                                       MeterExchange(mode, g, tr, m, &ex, srv.get(), conn.get())};
  t.exchange().set_mem(&mem);
  Down<decltype(t)> down{t};
  pl::BookStage<decltype(down)> stage{fc, down};
  m.t0 = stage.t_decode_ptr();
  pl::RunConfig rc;
  rc.chunk_bytes = 1U << 17;
  rc.queue_capacity = 1U << 14;
  rc.yield_every = 0;
  if (o.pipelined) pl::run_pipelined(src, stage, rc); else pl::run_single(src, stage, rc);

  RunOut r;
  if (srv) {
    net::Control c;
    c.kind = net::Control::Kind::Stop;
    while (!srv->post(c)) std::this_thread::yield();
    th.join();
    Samples t4(o.messages + 16);
    for (std::size_t i = 0; i < m.send_start.size(); ++i) {
      if (m.send_start[i] != 0 && m.arrive[i] > m.send_start[i]) t4.add(m.arrive[i] - m.send_start[i]);
    }
    r.t4 = summarize(t4, clock);
  }
  r.t1 = summarize(m.t1, clock);
  r.t2 = summarize(m.t2, clock);
  r.t2_first = summarize(m.t2_first, clock);
  r.t3 = summarize(m.t3, clock);
  r.t3_first = summarize(m.t3_first, clock);
  r.t5 = summarize(m.t5, clock);
  r.requests = t.digest().n_requests;
  r.fills = t.digest().n_fills;
  r.ok = !g.inconsistent();
  return r;
}

std::string first_line_with(const char* path, const char* key) {
  std::ifstream f(path);
  std::string line;
  while (std::getline(f, line)) {
    if (line.rfind(key, 0) == 0) return line.substr(line.find(':') + 1);
  }
  return "?";
}

void print_env(const Opts& o, const pl::ClockCalibration& clock) {
  utsname u{};
  uname(&u);
  const std::string flags = first_line_with("/proc/cpuinfo", "flags");
  std::printf("environment:\n");
  std::printf("  cpu:%s\n", first_line_with("/proc/cpuinfo", "model name").c_str());
  std::printf("  online cpus: %ld; kernel: %s %s; hypervisor flag: %s; constant_tsc: %s\n", sysconf(_SC_NPROCESSORS_ONLN),
              u.sysname, u.release, flags.find(" hypervisor") != std::string::npos ? "yes" : "no",
              flags.find(" constant_tsc") != std::string::npos ? "yes" : "no");
  std::printf("  compiler: %s; build: %s\n", __VERSION__,
#ifdef NDEBUG
              "optimized (NDEBUG)"
#else
              "debug"
#endif
  );
  std::printf("  threads: %s; pinned client %d server %d; exchange thread %s\n",
              o.pipelined ? "decoder + book + exchange" : "book + exchange", o.client_cpu, o.server_cpu,
              o.busy_poll ? "busy-polls epoll_wait(0)" : "sleeps in epoll_wait");
  std::printf("  clock: ticks overhead %.1f ns subtracted once per sample; %.4f ns/tick\n",
              clock.overhead_ticks * clock.ns_per_tick, clock.ns_per_tick);
}

void row(const char* mode, const char* seg, const Stats& s) {
  std::printf("| %-8s | %-34s | %8zu | %9.0f | %9.0f | %9.0f | %10.0f |\n", mode, seg, s.n, s.p50, s.p99, s.p999, s.max);
}

}  // namespace

int main(int argc, char** argv) {
  Opts o;
  for (int i = 1; i < argc; ++i) {
    const std::string k = argv[i];
    auto val = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", k.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (k == "--messages") o.messages = std::stoull(val());
    else if (k == "--seed") o.seed = std::stoull(val());
    else if (k == "--pipelined") o.pipelined = true;
    else if (k == "--quick") o.messages = 20'000;
    else if (k == "--input") o.input = val();
    else if (k == "--symbol") o.symbol = val();
    else if (k == "--client-cpu") o.client_cpu = std::stoi(val());
    else if (k == "--server-cpu") o.server_cpu = std::stoi(val());
    else if (k == "--busy-poll") o.busy_poll = true;
    else {
      std::fprintf(stderr, "lob_t2t: unknown option %s (see bench/tick_to_trade.cpp)\n", k.c_str());
      return 2;
    }
  }
  pin(o.client_cpu);
  md::FeedConfig fc;
  fc.book = md::BookConfig{1U << 20, 1U << 14};
  tk::Buf stream;
  if (o.input.empty()) {
    tk::GeneratorParams gp;
    gp.seed = o.seed;
    gp.messages = o.messages;
    stream = tk::generate_stream(gp);
    fc.symbol = md::make_symbol(tk::symbol_of(1));
  } else {
    if (o.symbol.empty()) {
      std::fprintf(stderr, "--input needs --symbol\n");
      return 2;
    }
    fc.symbol = md::make_symbol(o.symbol);
    fc.resume_grace_ns = 1'000'000'000ULL;
  }
  const pl::ClockCalibration clock = pl::calibrate();
  print_env(o, clock);
  std::printf("input: %s\n", o.input.empty() ? ("synthetic, " + std::to_string(o.messages) + " messages, seed " +
                                                std::to_string(o.seed)).c_str()
                                             : (o.input + " (" + o.symbol + ")").c_str());
  std::printf("\n| mode     | segment                            |  samples |   p50 ns |   p99 ns | p99.9 ns |     max ns |\n");
  std::printf("|---|---|---:|---:|---:|---:|---:|\n");
  bool ok = true;
  for (const Mode mode : {Mode::InProc, Mode::Loopback, Mode::Ack}) {
    std::unique_ptr<pl::ChunkSource> src;
    int fd = -1;
    if (o.input.empty()) {
      src = std::make_unique<pl::MemorySource>(std::span<const std::uint8_t>(stream));
    } else {
      fd = ::open(o.input.c_str(), O_RDONLY);
      if (fd < 0) {
        std::perror(o.input.c_str());
        return 1;
      }
      src = std::make_unique<pl::FdSource>(fd);
    }
    const RunOut r = run_mode(mode, o, *src, fc, clock);
    if (fd >= 0) ::close(fd);
    ok = ok && r.ok;
    if (mode == Mode::InProc) {
      row("inproc", "T1 tick -> decision (new order)", r.t1);
      row("inproc", "T2 tick -> encoded, new order", r.t2);
      row("inproc", "T2 tick -> encoded, 1st msg", r.t2_first);
    } else if (mode == Mode::Loopback) {
      row("loopback", "T1 tick -> decision (new order)", r.t1);
      row("loopback", "T3 tick -> send() ret, new order", r.t3);
      row("loopback", "T3 tick -> send() ret, 1st msg", r.t3_first);
      row("loopback", "T4 client send -> exchange decoded", r.t4);
    } else {
      row("ack", "T5 tick -> ack decoded (round trip)", r.t5);
    }
    std::fprintf(stderr, "  [%s] requests %llu fills %llu\n",
                 mode == Mode::InProc ? "inproc" : mode == Mode::Loopback ? "loopback" : "ack",
                 static_cast<unsigned long long>(r.requests), static_cast<unsigned long long>(r.fills));
  }
  if (!ok) {
    std::fprintf(stderr, "FATAL: the gateway became inconsistent\n");
    return 1;
  }
  return 0;
}
