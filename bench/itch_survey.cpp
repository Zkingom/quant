// lob_itch_survey: one-pass diagnostic survey of a real ITCH 5.0 stream for
// one instrument (real-data validation of D-054). Reads stdin in fixed
// chunks (bounded memory, nothing written to disk), e.g.
//
//   curl -sS URL | gzip -dc | lob_itch_survey --symbol AAPL
//
// Reports:
//   - frame counts per message type (whole stream, and the instrument);
//   - System Events, the instrument's Trading Actions, cross trades (Q) and
//     NOII (I) summaries, with timestamps;
//   - the production FeedHandler: final state, counters, every alert with
//     the instrument-relevant messages before and after it;
//   - a SHADOW analysis that keeps going after the first problem: an
//     md::Book kept in non-strict mode with its own phase tracking records
//     every point where the D-054 rules WITHOUT the D-055 resume grace would
//     go Stale -- strict mode entered on a locked/crossed book, a locking/crossing add
//     or replace in strict mode, any other book rejection -- with context,
//     and how long a crossed book at a strict transition took to clear;
//   - book updates per trading phase, locked/crossed counts per phase;
//   - every intraday Trading Action of EVERY instrument (state changes after
//     its first 'T', or any non-'T' state), to find halted instruments;
//   - optionally (--series CSV) the instrument's best bid / ask / cross state
//     after every update around each regular-hours pause or halt (charts);
//   - peak RSS and elapsed time.
// Not on any hot path; not part of CTest. scripts/itch_real_survey.sh runs it
// together with lob_itch (pipeline and single-threaded) on one download.
// Results of 2026-11-04: MODULE_NOTES/L3.md "真实数据验证".

#include <sys/resource.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <type_traits>
#include <variant>
#include <string>
#include <vector>

#include "lob/md/book.hpp"
#include "lob/md/feed.hpp"
#include "lob/md/itch.hpp"

namespace {

namespace md = lob::md;
namespace itch = lob::md::itch;

std::string hms(std::uint64_t ns) {
  const std::uint64_t s = ns / 1'000'000'000ULL;
  char b[48];
  std::snprintf(b, sizeof b, "%02llu:%02llu:%02llu.%09llu", static_cast<unsigned long long>(s / 3600),
                static_cast<unsigned long long>(s / 60 % 60), static_cast<unsigned long long>(s % 60),
                static_cast<unsigned long long>(ns % 1'000'000'000ULL));
  return b;
}

std::string px(std::uint32_t p) {
  char b[32];
  std::snprintf(b, sizeof b, "%u.%04u", p / 10000, p % 10000);
  return b;
}

struct Options {
  std::string series;  ///< CSV of the book around every pause / halt (see --series)
  std::string symbol;
  std::size_t context{12};
  std::size_t max_points{60};
};

/// One instrument-relevant frame, rendered for context windows.
struct Line {
  std::uint64_t seq;
  std::string text;
};

class Survey {
 public:
  explicit Survey(const Options& o) : o_(o), sym_(md::make_symbol(o.symbol)) {
    md::FeedConfig cfg;
    cfg.symbol = sym_;
    cfg.book = md::BookConfig{1U << 20, 1U << 14};
    feed_ = std::make_unique<md::FeedHandler>(cfg);
    shadow_.set_strict(false);
    events_.reserve(64);
  }

  void frame(std::uint64_t seq, itch::Bytes p) {
    ++frames_;
    const char type = p.empty() ? '?' : static_cast<char>(p[0]);
    ++by_type_[type];
    const bool ours_locate = have_locate_ && p.size() >= 3 && itch::be16(p.data() + 1) == locate_;
    if (ours_locate) ++ours_by_type_[type];

    md::FeedInput in{};
    in.seq = seq;
    const auto ds = itch::decode(p, in.msg);
    in.kind = ds == itch::DecodeStatus::Decoded   ? md::InputKind::Decoded
              : ds == itch::DecodeStatus::Skipped ? md::InputKind::Skipped
                                                  : md::InputKind::DecodeError;
    if (in.kind == md::InputKind::DecodeError) {
      in.error = static_cast<std::uint8_t>(ds);
      ++decode_errors_;
    }

    // Context line for anything relevant to the instrument or the phase.
    std::string line = describe(seq, p, in, ds, ours_locate);
    if (!line.empty()) push_context(seq, line);

    // Production handler.
    events_.clear();
    feed_->on_input(in, events_);
    for (const auto& e : events_) {
      if (e.kind == md::FeedEvent::Kind::StateChange || e.kind == md::FeedEvent::Kind::Alert) {
        alert(e);
      }
    }
    // Shadow analysis.
    if (in.kind == md::InputKind::Decoded) shadow(seq, in);
    pending_after();
  }

  void write_series() const {
    if (o_.series.empty()) return;
    std::FILE* f = std::fopen(o_.series.c_str(), "w");
    if (f == nullptr) return;
    std::fprintf(f, "# lob_itch_survey --series: %s book after every update from each strict-mode exit until 1 ms after "
                    "the next clean strict entry\n# ts_ns,event,bid,ask,cross,seq\n", o_.symbol.c_str());
    for (const auto& l : series_) std::fprintf(f, "%s\n", l.c_str());
    std::fclose(f);
  }

  void finish(double secs, std::uint64_t bytes, bool truncated) {
    write_series();
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    std::printf("\n# Survey of %s\n\n", o_.symbol.c_str());
    std::printf("input: %llu bytes, %llu frames, decode errors %llu, trailing truncation %s\n",
                static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(frames_),
                static_cast<unsigned long long>(decode_errors_), truncated ? "YES" : "no");
    std::printf("elapsed %.1f s, peak RSS %ld KiB\n", secs, ru.ru_maxrss);
    std::printf("locate of %s: %s\n", o_.symbol.c_str(), have_locate_ ? std::to_string(locate_).c_str() : "not found");

    std::printf("\n## Frames per message type (all instruments | %s)\n\n| type | all | %s |\n|---|---|---|\n",
                o_.symbol.c_str(), o_.symbol.c_str());
    for (const auto& [t, n] : by_type_) {
      const auto it = ours_by_type_.find(t);
      std::printf("| %c | %llu | %llu |\n", t, static_cast<unsigned long long>(n),
                  static_cast<unsigned long long>(it == ours_by_type_.end() ? 0 : it->second));
    }

    std::printf("\n## System Events\n\n");
    for (const auto& s : system_log_) std::printf("- %s\n", s.c_str());
    std::printf("\n## %s Trading Actions\n\n", o_.symbol.c_str());
    for (const auto& s : trading_log_) std::printf("- %s\n", s.c_str());
    std::printf("\n## Intraday Trading Actions, all instruments (%llu messages; instruments with more than one H "
                "or a non-'T' state)\n\n", u(intraday_h_));
    for (const auto& [stock, hist] : halts_) {
      if (hist.size() < 2 && hist.front().find(" T '") != std::string::npos) continue;
      std::printf("- %s:", stock.c_str());
      for (const auto& h : hist) std::printf(" [%s]", h.c_str());
      std::printf("\n");
    }
    std::printf("\n## %s cross trades (Q)\n\n", o_.symbol.c_str());
    for (const auto& s : crosses_) std::printf("- %s\n", s.c_str());
    std::printf("\n## %s NOII (I): %llu messages\n\n", o_.symbol.c_str(), static_cast<unsigned long long>(noii_n_));
    for (const auto& [ct, r] : noii_) {
      std::printf("- cross type %c: %llu messages, %s .. %s\n", ct, static_cast<unsigned long long>(r.n),
                  hms(r.first).c_str(), hms(r.last).c_str());
    }

    const auto& c = feed_->counters();
    std::printf("\n## Production FeedHandler (D-054 rules)\n\nfinal state %s, session %c, trading %c, strict %d\n",
                std::string(md::to_string(feed_->state())).c_str(), feed_->session() ? feed_->session() : '-',
                feed_->trading_state() ? feed_->trading_state() : '-', feed_->strict() ? 1 : 0);
    std::printf("applied %llu, other_stock %llu, skipped %llu, duplicates %llu, gaps %llu, ignored_not_live %llu, "
                "locked_updates %llu, crossed_updates %llu, session_events %llu, trading_actions %llu, "
                "cross_trades %llu, resume_graces %llu, graces_resolved %llu\n",
                u(c.applied), u(c.other_stock), u(c.skipped), u(c.duplicates), u(c.gaps), u(c.ignored_not_live),
                u(c.locked_updates), u(c.crossed_updates), u(c.session_events), u(c.trading_actions),
                u(c.cross_trades), u(c.graces), u(c.graces_resolved));
    std::printf("book: orders %zu, bid levels %zu, ask levels %zu, hash 0x%016llx\n", feed_->book().order_count(),
                feed_->book().level_count(md::Side::Buy), feed_->book().level_count(md::Side::Sell),
                static_cast<unsigned long long>(feed_->book().state_hash()));
    std::printf("\nalerts / state changes (%zu):\n", alerts_.size());
    for (const auto& a : alerts_) std::printf("%s\n", a.c_str());

    std::printf("\n## Shadow analysis: %llu points where the D-054 rules go Stale (marked *; transitions and "
                "resolutions listed too; first %zu entries)\n",
                u(points_total_), points_.size());
    for (const auto& p : points_) std::printf("%s\n", p.c_str());

    std::printf("\n## %s book updates per phase (shadow book, non-strict throughout)\n\n"
                "| session | trading | updates | locked after | crossed after |\n|---|---|---|---|---|\n",
                o_.symbol.c_str());
    for (const auto& [k, v] : phase_) {
      std::printf("| %c | %c | %llu | %llu | %llu |\n", k.first ? k.first : '-', k.second ? k.second : '-', u(v[0]),
                  u(v[1]), u(v[2]));
    }
    std::fflush(stdout);
  }

 private:
  static unsigned long long u(std::uint64_t v) { return static_cast<unsigned long long>(v); }

  std::string describe(std::uint64_t seq, itch::Bytes p, const md::FeedInput& in, itch::DecodeStatus ds,
                       bool ours_locate) {
    char b[256];
    if (p.size() < itch::kHeaderSize) return "";
    const char t = static_cast<char>(p[0]);
    const std::uint64_t ts = itch::be48(p.data() + 5);
    if (ds == itch::DecodeStatus::Decoded) {
      if (const auto* se = std::get_if<itch::SystemEvent>(&in.msg)) {
        std::snprintf(b, sizeof b, "%s S event %c", hms(ts).c_str(), se->event_code);
        system_log_.push_back(b);
        return b;
      }
      if (const auto* d = std::get_if<itch::StockDirectory>(&in.msg)) {
        if (d->stock == sym_ && !have_locate_) {
          have_locate_ = true;
          locate_ = d->header.stock_locate;
          std::snprintf(b, sizeof b, "%s R directory locate %u", hms(ts).c_str(), locate_);
          return b;
        }
        return "";
      }
      if (const auto* ta = std::get_if<itch::TradingAction>(&in.msg)) {
        auto& hist = halts_[std::string(ta->stock.data(), 8)];
        const bool intraday = !hist.empty() || ta->trading_state != 'T';
        hist.push_back(hms(ts) + " " + ta->trading_state + " '" + std::string(ta->reason.data(), 4) + "'");
        if (intraday) ++intraday_h_;
        if (ta->stock != sym_) return "";
        std::snprintf(b, sizeof b, "%s H state %c reason '%.4s'", hms(ts).c_str(), ta->trading_state, ta->reason.data());
        trading_log_.push_back(b);
        return b;
      }
    }
    if (!ours_locate) return "";
    if (t == 'Q' && p.size() == 40) {
      std::snprintf(b, sizeof b, "%s Q cross trade type %c shares %llu price %s", hms(ts).c_str(),
                    static_cast<char>(p[39]), u(itch::be64(p.data() + 11)), px(itch::be32(p.data() + 27)).c_str());
      crosses_.push_back(b);
      return b;
    }
    if (t == 'I' && p.size() == 50) {
      ++noii_n_;
      auto& r = noii_[static_cast<char>(p[48])];
      if (r.n++ == 0) r.first = ts;
      r.last = ts;
      return "";  // too many to list; summarised
    }
    if (t == 'P' && p.size() == 44) {
      std::snprintf(b, sizeof b, "%s P trade (non-displayed) %c %u @ %s", hms(ts).c_str(), static_cast<char>(p[19]),
                    itch::be32(p.data() + 20), px(itch::be32(p.data() + 32)).c_str());
      return b;
    }
    if (ds != itch::DecodeStatus::Decoded) {
      std::snprintf(b, sizeof b, "%s %c (%s)", hms(ts).c_str(), t, std::string(itch::to_string(ds)).c_str());
      return b;
    }
    std::string s = hms(ts) + " ";
    std::visit(
        [&](const auto& m) {
          using T = std::decay_t<decltype(m)>;
          if constexpr (std::is_same_v<T, itch::AddOrder>) {
            std::snprintf(b, sizeof b, "%c add ref %llu %c %u @ %s", t, u(m.order_ref), static_cast<char>(m.side),
                          m.shares, px(m.price).c_str());
          } else if constexpr (std::is_same_v<T, itch::OrderExecuted>) {
            std::snprintf(b, sizeof b, "E exec ref %llu %u", u(m.order_ref), m.executed_shares);
          } else if constexpr (std::is_same_v<T, itch::OrderExecutedWithPrice>) {
            std::snprintf(b, sizeof b, "C exec ref %llu %u @ %s printable %c", u(m.order_ref), m.executed_shares,
                          px(m.execution_price).c_str(), m.printable);
          } else if constexpr (std::is_same_v<T, itch::OrderCancel>) {
            std::snprintf(b, sizeof b, "X cancel ref %llu %u", u(m.order_ref), m.cancelled_shares);
          } else if constexpr (std::is_same_v<T, itch::OrderDelete>) {
            std::snprintf(b, sizeof b, "D delete ref %llu", u(m.order_ref));
          } else if constexpr (std::is_same_v<T, itch::OrderReplace>) {
            std::snprintf(b, sizeof b, "U replace ref %llu -> %llu %u @ %s", u(m.original_order_ref), u(m.new_order_ref),
                          m.shares, px(m.price).c_str());
          } else {
            std::snprintf(b, sizeof b, "%c", t);
          }
        },
        in.msg);
    s += b;
    (void)seq;
    return s;
  }

  std::string bbo() const {
    char b[128];
    const auto bb = shadow_.best(md::Side::Buy);
    const auto aa = shadow_.best(md::Side::Sell);
    std::snprintf(b, sizeof b, "bbo %s x %llu | %s x %llu", bb ? px(bb->price).c_str() : "-", bb ? u(bb->shares) : 0ULL,
                  aa ? px(aa->price).c_str() : "-", aa ? u(aa->shares) : 0ULL);
    return b;
  }

  void push_context(std::uint64_t seq, const std::string& text) {
    ring_.push_back(Line{seq, text});
    if (ring_.size() > o_.context) ring_.pop_front();
    for (auto& w : waiting_) {
      if (w.remaining > 0 && seq > w.after_seq) {
        (*w.sink)[w.index].append("      + #" + std::to_string(seq) + " " + text + "\n");
        --w.remaining;
      }
    }
  }

  void pending_after() {
    while (!waiting_.empty() && waiting_.front().remaining == 0) waiting_.pop_front();
  }

  std::string context_before(std::uint64_t seq) const {
    std::string s;
    for (const auto& l : ring_) {
      s += (l.seq == seq ? "      > #" : "      - #") + std::to_string(l.seq) + " " + l.text + "\n";
    }
    return s;
  }

  /// Records a point; its context "after" fills in as later frames arrive.
  void record(std::vector<std::string>& sink, std::uint64_t seq, const std::string& head) {
    sink.push_back(head + "\n" + context_before(seq));
    waiting_.push_back(Wait{&sink, sink.size() - 1, seq, o_.context});
  }

  void alert(const md::FeedEvent& e) {
    std::string head = "  [" + hms(e.timestamp) + "] seq " + std::to_string(e.seq) + " " +
                       std::string(md::to_string(e.state)) + " reason " + std::string(md::to_string(e.reason)) +
                       " detail " + std::to_string(e.detail) + " sess " + (e.session ? std::string(1, e.session) : "-") +
                       " trd " + (e.trading ? std::string(1, e.trading) : "-") + " cross " + std::string(1, e.cross);
    if (alerts_.size() < 50) record(alerts_, e.seq, head);
  }

  // ---------------------------------------------------------------- shadow

  bool strict_phase() const { return session_ == 'Q' && trading_ == 'T'; }

  /// `stale`: the production rules would go Stale here (counted); otherwise
  /// an informational transition / resolution line.
  void point(std::uint64_t seq, std::uint64_t ts, const std::string& what, bool stale = true) {
    if (stale) ++points_total_;
    if (points_.size() < o_.max_points) {
      record(points_, seq, std::string(stale ? "  * [" : "  · [") + hms(ts) + "] seq " + std::to_string(seq) + " " + what + "; " + bbo());
    }
  }

  void shadow(std::uint64_t seq, const md::FeedInput& in) {
    const auto& h = itch::header_of(in.msg);
    if (const auto* se = std::get_if<itch::SystemEvent>(&in.msg)) {
      const bool was = strict_phase();
      session_ = se->event_code;
      transition(seq, h.timestamp, was, "S " + std::string(1, se->event_code));
      return;
    }
    if (const auto* ta = std::get_if<itch::TradingAction>(&in.msg)) {
      if (ta->stock != sym_) return;
      point(seq, h.timestamp,
            std::string("H ") + ta->trading_state + " reason '" + std::string(ta->reason.data(), 4) + "' (book " +
                (shadow_.cross_state() == md::CrossState::None
                     ? "clean"
                     : shadow_.cross_state() == md::CrossState::Locked ? "LOCKED" : "CROSSED") +
                ", " + std::to_string(shadow_.order_count()) + " orders)",
            false);
      const bool was = strict_phase();
      trading_ = ta->trading_state;
      transition(seq, h.timestamp, was, "H " + std::string(1, ta->trading_state));
      return;
    }
    if (!have_locate_ || h.stock_locate != locate_) return;
    if (std::holds_alternative<itch::StockDirectory>(in.msg)) return;

    // Would this add/replace lock or cross in strict mode?
    std::uint32_t new_px = 0;
    md::Side new_side = md::Side::Buy;
    bool is_new = false;
    if (const auto* a = std::get_if<itch::AddOrder>(&in.msg)) {
      new_px = a->price;
      new_side = a->side;
      is_new = true;
    } else if (const auto* r = std::get_if<itch::OrderReplace>(&in.msg)) {
      if (const auto o = shadow_.find(r->original_order_ref)) {
        new_px = r->price;
        new_side = o->side;
        is_new = true;
      }
    }
    if (is_new && strict_phase()) {
      const auto opp = shadow_.best(new_side == md::Side::Buy ? md::Side::Sell : md::Side::Buy);
      if (opp && (new_side == md::Side::Buy ? new_px >= opp->price : new_px <= opp->price)) {
        point(seq, h.timestamp, std::string("strict-mode ") + (new_px == opp->price ? "LOCKING" : "CROSSING") + " " +
                                    static_cast<char>(new_side) + " at " + px(new_px));
      }
    }
    md::BookStatus st = md::BookStatus::Applied;
    std::visit(
        [&](const auto& m) {
          using T = std::decay_t<decltype(m)>;
          if constexpr (std::is_same_v<T, itch::AddOrder>) st = shadow_.add(m.order_ref, m.side, m.shares, m.price);
          else if constexpr (std::is_same_v<T, itch::OrderExecuted>) st = shadow_.execute(m.order_ref, m.executed_shares);
          else if constexpr (std::is_same_v<T, itch::OrderExecutedWithPrice>)
            st = shadow_.execute(m.order_ref, m.executed_shares);
          else if constexpr (std::is_same_v<T, itch::OrderCancel>) st = shadow_.cancel(m.order_ref, m.cancelled_shares);
          else if constexpr (std::is_same_v<T, itch::OrderDelete>) st = shadow_.remove(m.order_ref);
          else if constexpr (std::is_same_v<T, itch::OrderReplace>)
            st = shadow_.replace(m.original_order_ref, m.new_order_ref, m.shares, m.price);
        },
        in.msg);
    if (st != md::BookStatus::Applied) {
      point(seq, h.timestamp, "book rejected: " + std::string(md::to_string(st)));
      return;
    }
    const md::CrossState cs = shadow_.cross_state();
    if (sampling_) {
      sample(seq, h.timestamp, std::string(1, h.type));
      if (sample_until_ == 0 && strict_phase() && cs == md::CrossState::None) sample_until_ = h.timestamp + 1'000'000;
      if (sample_until_ != 0 && h.timestamp > sample_until_) sampling_ = false;
    }
    auto& v = phase_[{session_, trading_}];
    ++v[0];
    if (cs == md::CrossState::Locked) ++v[1];
    if (cs == md::CrossState::Crossed) ++v[2];
    if (resolving_ && cs == md::CrossState::None) {
      resolving_ = false;
      point(seq, h.timestamp,
            "RESOLVED: crossed/locked book at the strict transition cleared after " +
                std::to_string(seq - resolve_seq_) + " frames, " + std::to_string((h.timestamp - resolve_ts_) / 1000) +
                " us", false);
    }
  }

  void sample(std::uint64_t seq, std::uint64_t ts, const std::string& event) {
    const auto bb = shadow_.best(md::Side::Buy);
    const auto aa = shadow_.best(md::Side::Sell);
    const md::CrossState cs = shadow_.cross_state();
    char b[160];
    std::snprintf(b, sizeof b, "%llu,%s,%u,%u,%c,%llu", u(ts), event.c_str(), bb ? bb->price : 0U, aa ? aa->price : 0U,
                  cs == md::CrossState::None ? '-' : (cs == md::CrossState::Locked ? 'L' : 'X'), u(seq));
    series_.push_back(b);
  }

  void transition(std::uint64_t seq, std::uint64_t ts, bool was_strict, const std::string& what) {
    const bool now = strict_phase();
    if (!now && was_strict && session_ == 'Q') {
      sampling_ = true;  // a pause / halt in regular hours: record until the book is clean in strict mode again
      sample_until_ = 0;
    }
    if (sampling_) sample(seq, ts, what);
    if (now && !was_strict) {
      const md::CrossState cs = shadow_.cross_state();
      if (cs != md::CrossState::None) {
        point(seq, ts, "strict mode entered by " + what + " on a " +
                           (cs == md::CrossState::Locked ? "LOCKED" : "CROSSED") +
                           " book (D-054 alone: CROSSED_AT_RESUME; with the D-055 grace: RESUME_GRACE)");
        resolving_ = true;
        resolve_seq_ = seq;
        resolve_ts_ = ts;
      } else {
        point(seq, ts, "strict mode entered by " + what + " on a clean book (ok)", false);
      }
    } else if (!now && was_strict) {
      point(seq, ts, "strict mode left by " + what + " (ok)", false);
      resolving_ = false;
    }
  }

  struct Wait {
    std::vector<std::string>* sink;
    std::size_t index;
    std::uint64_t after_seq;
    std::size_t remaining;
  };
  struct Range {
    std::uint64_t n{0}, first{0}, last{0};
  };

  Options o_;
  itch::Symbol sym_;
  std::unique_ptr<md::FeedHandler> feed_;
  std::vector<md::FeedEvent> events_;
  md::Book shadow_{md::BookConfig{1U << 20, 1U << 14}};
  char session_{0};
  char trading_{0};
  bool have_locate_{false};
  std::uint16_t locate_{0};
  std::uint64_t frames_{0}, decode_errors_{0}, noii_n_{0}, points_total_{0};
  std::map<char, std::uint64_t> by_type_, ours_by_type_;
  std::map<char, Range> noii_;
  std::map<std::string, std::vector<std::string>> halts_;  // all instruments
  std::uint64_t intraday_h_{0};
  std::vector<std::string> system_log_, trading_log_, crosses_, alerts_, points_;
  std::map<std::pair<char, char>, std::array<std::uint64_t, 3>> phase_;
  std::deque<Line> ring_;
  std::deque<Wait> waiting_;
  bool resolving_{false};
  bool sampling_{false};
  std::uint64_t sample_until_{0};
  std::vector<std::string> series_;
  std::uint64_t resolve_seq_{0}, resolve_ts_{0};
};

}  // namespace

int main(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string k = argv[i];
    if (k == "--symbol" && i + 1 < argc) o.symbol = argv[++i];
    else if (k == "--context" && i + 1 < argc) o.context = std::stoul(argv[++i]);
    else if (k == "--max-points" && i + 1 < argc) o.max_points = std::stoul(argv[++i]);
    else if (k == "--series" && i + 1 < argc) o.series = argv[++i];
    else {
      std::fprintf(stderr, "usage: lob_itch_survey --symbol SYM [--context N] [--max-points N] [--series CSV] < itch_stream\n");
      return 2;
    }
  }
  if (o.symbol.empty()) {
    std::fprintf(stderr, "--symbol is required\n");
    return 2;
  }
  const auto t0 = std::chrono::steady_clock::now();
  Survey s{o};
  std::vector<std::uint8_t> buf(1U << 20);
  std::size_t filled = 0;
  std::uint64_t seq = 1, bytes = 0;
  bool eof = false;
  for (;;) {
    if (!eof) {
      const ssize_t r = ::read(0, buf.data() + filled, buf.size() - filled);
      if (r <= 0) eof = true; else { filled += static_cast<std::size_t>(r); bytes += static_cast<std::uint64_t>(r); }
    }
    itch::FrameCursor cur{itch::Bytes{buf.data(), filled}};
    itch::Bytes p;
    itch::FrameStatus fs;
    while ((fs = cur.next(p)) == itch::FrameStatus::Frame) s.frame(seq++, p);
    std::memmove(buf.data(), buf.data() + cur.offset(), filled - cur.offset());
    filled -= cur.offset();
    if (fs == itch::FrameStatus::ZeroLength || (eof && filled == 0) || (eof && fs == itch::FrameStatus::Truncated)) break;
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  s.finish(secs, bytes, filled != 0);
  return 0;
}
