#include "lob/testkit/differential.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <new>
#include <set>
#include <sstream>
#include <variant>

#include "lob/engine/matching_engine.hpp"
#include "lob/testkit/describe.hpp"
#include "lob/testkit/reference_engine.hpp"

namespace lob::testkit {

// ------------------------------------------------------------------ Coverage

void Coverage::merge(const Coverage& o) {
  commands += o.commands;
  accepted += o.accepted;
  trades += o.trades;
  cancels += o.cancels;
  multi_maker_commands += o.multi_maker_commands;
  multi_level_commands += o.multi_level_commands;
  taker_fully_filled += o.taker_fully_filled;
  taker_partial_then_rested += o.taker_partial_then_rested;
  rested_without_trade += o.rested_without_trade;
  maker_partially_filled += o.maker_partially_filled;
  accepted_while_full += o.accepted_while_full;
  reused_dead_id += o.reused_dead_id;
  for (int i = 0; i < 8; ++i) {
    rejects[i] += o.rejects[i];
  }
  max_orders = std::max(max_orders, o.max_orders);
  max_trades_in_command = std::max(max_trades_in_command, o.max_trades_in_command);
  max_levels_swept = std::max(max_levels_swept, o.max_levels_swept);
  side_emptied += o.side_emptied;
  band_edge_accepts += o.band_edge_accepts;
  band_edge_trades += o.band_edge_trades;
  max_levels_per_side = std::max(max_levels_per_side, o.max_levels_per_side);
  max_level_orders = std::max(max_level_orders, o.max_level_orders);
  max_level_quantity = std::max(max_level_quantity, o.max_level_quantity);
  accepted_quantity += o.accepted_quantity;
  full_checks += o.full_checks;
  faults_prepare += o.faults_prepare;
  faults_residual += o.faults_residual;
  faults_armed_without_effect += o.faults_armed_without_effect;
}

std::string Coverage::summary() const {
  std::ostringstream s;
  s << "[coverage] commands=" << commands << " accepted=" << accepted << " trades=" << trades
    << " cancels=" << cancels << '\n'
    << "[coverage] multi_maker=" << multi_maker_commands << " multi_level=" << multi_level_commands
    << " taker_full=" << taker_fully_filled << " taker_partial_rested=" << taker_partial_then_rested
    << " rested_no_trade=" << rested_without_trade << " maker_partial=" << maker_partially_filled
    << '\n'
    << "[coverage] accepted_while_full=" << accepted_while_full
    << " reused_dead_id=" << reused_dead_id << " max_orders=" << max_orders << '\n'
    << "[coverage] rejects:";
  for (int i = 0; i < 8; ++i) {
    s << ' ' << to_string(static_cast<RejectReason>(i)) << '=' << rejects[i];
  }
  s << '\n'
    << "[state] max_levels_per_side=" << max_levels_per_side
    << " max_level_orders=" << max_level_orders << " max_level_quantity=" << max_level_quantity
    << " max_trades_in_command=" << max_trades_in_command
    << " max_levels_swept=" << max_levels_swept << " side_emptied=" << side_emptied
    << " band_edge_accepts=" << band_edge_accepts << " band_edge_trades=" << band_edge_trades
    << " accepted_quantity=" << to_string(accepted_quantity) << " full_checks=" << full_checks
    << '\n'
    << "[faults] prepare=" << faults_prepare << " residual=" << faults_residual
    << " armed_without_effect=" << faults_armed_without_effect << '\n';
  return s.str();
}

// --------------------------------------------------------------------- runner

namespace {

std::vector<ReferenceEngine::Resting> engine_side(const OrderBook& book, Side side) {
  std::vector<ReferenceEngine::Resting> out;
  book.for_each_order(side, [&](Price price, const RestingOrder& o) {
    out.push_back(ReferenceEngine::Resting{o.id, side, price, o.remaining, o.seq});
  });
  return out;
}

class Runner {
 public:
  Runner(const WorkloadProfile& profile, const RunOptions& options)
      : profile_(profile),
        options_(options),
        engine_(profile.config),
        ref_(profile.config),
        auditor_(profile.config),
        generator_(profile, options.seed),
        // Independent stream: arming faults never perturbs the command stream.
        fault_rng_(options.seed * 0x9E3779B97F4A7C15ULL + 0x2545F4914F6CDD1DULL) {}

  RunReport run() {
    const auto start = std::chrono::steady_clock::now();
    for (step_ = 0; step_ < options_.commands && report_.ok; ++step_) {
      one_command();
      if (report_.ok && (step_ + 1) % options_.full_check_every == 0) {
        full_check();
      }
    }
    if (report_.ok) {
      full_check();
    }
    cov_.accepted_quantity = auditor_.accepted_quantity();
    report_.coverage = cov_;
    report_.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return report_;
  }

 private:
  void fail(const std::string& what, const Command* command = nullptr,
            const std::vector<Event>* actual = nullptr, const std::vector<Event>* expected = nullptr) {
    if (!report_.ok) {
      return;
    }
    report_.ok = false;
    report_.failed_step = step_;
    std::ostringstream s;
    s << "step " << step_ << ": " << what;
    if (command != nullptr) {
      s << "\ncommand: " << describe(*command);
    }
    if (actual != nullptr) {
      s << "\nengine:\n" << describe(*actual);
    }
    if (expected != nullptr) {
      s << "reference:\n" << describe(*expected);
    }
    report_.failure = s.str();
  }

  void one_command() {
    const Command command = generator_.next(step_, ref_);
    const bool was_full =
        static_cast<std::int64_t>(ref_.order_count()) >= profile_.config.max_live_orders;

    bool armed = false;
    if (options_.fault_per_mille > 0 && fault_rng_.uniform(0, 999) < options_.fault_per_mille) {
      armed = true;
      fault_k_ = fault_rng_.uniform(0, 5);
      if (fault_rng_.percent(50)) {
        std::vector<Event>().swap(events_);  // zero capacity: the engine must grow it
      }
    }
    events_.clear();
    const SeqNum seq_before = engine_.last_seq();

    bool threw = false;
    try {
      if (armed) {
        options_.injector->arm(fault_k_);
      }
      engine_.process(command, events_);
    } catch (const std::bad_alloc&) {
      threw = true;
    } catch (const std::exception& e) {
      if (armed) {
        options_.injector->disarm();
      }
      fail(std::string("unexpected exception: ") + e.what(), &command, &events_);
      return;
    }
    if (armed) {
      const bool injected = options_.injector->injected();
      options_.injector->disarm();
      if (!threw && injected) {
        fail("an injected allocation failure was swallowed", &command, &events_);
        return;
      }
      if (threw && !injected) {
        fail("bad_alloc without injection", &command, &events_);
        return;
      }
      if (!injected) {
        ++cov_.faults_armed_without_effect;
      }
    } else if (threw) {
      fail("bad_alloc without injection", &command, &events_);
      return;
    }

    if (threw) {
      handle_fault(command, seq_before);
      return;
    }

    const std::vector<Event> expected = ref_.process(command);
    if (events_ != expected) {
      fail("event streams differ", &command, &events_, &expected);
      return;
    }
    if (auto v = auditor_.audit(command, events_)) {
      fail("audit: " + *v, &command, &events_);
      return;
    }
    record(command, was_full);
  }

  void handle_fault(const Command& command, SeqNum seq_before) {
    if (events_.empty() && engine_.last_seq() == seq_before) {
      // PREPARE: nothing happened; the reference skips the command. The full
      // check below proves the book is unchanged (the reference did not move).
      ++cov_.faults_prepare;
      full_check();
      return;
    }
    // RESIDUAL.
    std::vector<Event> expected = ref_.process(command);
    const auto* rested = expected.empty() ? nullptr : std::get_if<OrderRested>(&expected.back());
    if (rested == nullptr) {
      fail("events changed under a failure, but the reference does not rest the order", &command,
           &events_, &expected);
      return;
    }
    const OrderRested r = *rested;
    expected.back() = OrderCancelled{.seq = r.seq,
                                     .id = r.id,
                                     .side = r.side,
                                     .price = r.price,
                                     .remaining = r.remaining,
                                     .reason = CancelReason::InsufficientResources};
    if (events_ != expected) {
      fail("RESIDUAL outcome differs from the reference", &command, &events_, &expected);
      return;
    }
    if (engine_.last_seq() != r.seq) {
      fail("last_seq after RESIDUAL is not the CANCELLED seq", &command, &events_);
      return;
    }
    ref_.drop_resting(r.id);
    if (auto v = auditor_.audit(command, events_)) {
      fail("audit after RESIDUAL: " + *v, &command, &events_);
      return;
    }
    ++cov_.faults_residual;
    full_check();
  }

  void full_check() {
    ++cov_.full_checks;
    for (const Side side : {Side::Buy, Side::Sell}) {
      if (engine_side(engine_.book(), side) != ref_.side_in_priority(side)) {
        fail(std::string(to_string(side)) + " books differ\nengine book:\n" + dump(engine_.book()));
        return;
      }
    }
    if (auto v = auditor_.matches(engine_.book())) {
      fail("audit book: " + *v);
      return;
    }
    if (const auto v = engine_.find_invariant_violation()) {
      fail("invariant: " + *v);
      return;
    }
    for (const Side side : {Side::Buy, Side::Sell}) {
      const auto levels = engine_.book().depth(side, std::numeric_limits<std::size_t>::max());
      cov_.max_levels_per_side = std::max(cov_.max_levels_per_side, levels.size());
      for (const LevelSummary& l : levels) {
        cov_.max_level_orders = std::max(cov_.max_level_orders, l.order_count);
        cov_.max_level_quantity = std::max(cov_.max_level_quantity, l.total_quantity);
      }
    }
  }

  // Queues and level totals build up at the touch and are often swept within
  // a few commands, so sampling them only at full checks misses the peaks
  // (M4 finding). The best level of each side is sampled after every accepted
  // order; the full-depth level count is sampled at full checks.
  void sample_best_levels() {
    for (const Side side : {Side::Buy, Side::Sell}) {
      for (const LevelSummary& l : engine_.book().depth(side, 1)) {
        cov_.max_level_orders = std::max(cov_.max_level_orders, l.order_count);
        cov_.max_level_quantity = std::max(cov_.max_level_quantity, l.total_quantity);
      }
    }
  }

  // Counters as in the M3 differential test (same definitions), plus the
  // M4 state metrics.
  void record(const Command& command, bool was_full) {
    ++cov_.commands;
    cov_.max_orders = std::max(cov_.max_orders, ref_.order_count());
    if (const auto* r = std::get_if<OrderRejected>(&events_.front())) {
      ++cov_.rejects[static_cast<int>(r->reason)];
      if (r->reason == RejectReason::BookFull) {
        // DESIGN §3.4: BOOK_FULL only refuses orders that could not trade.
        // A rejection changes nothing, so the current reference state is
        // the state the order saw.
        const auto& o = std::get<NewOrder>(command);
        if (const auto best = ref_.best_price(opposite(o.side));
            best && (o.side == Side::Buy ? *best <= o.price : *best >= o.price)) {
          fail("BOOK_FULL refused an order that could trade", &command, &events_);
        }
      }
      return;
    }
    if (std::holds_alternative<OrderCancelled>(events_.front())) {
      ++cov_.cancels;
      return;
    }
    const auto& accepted = std::get<OrderAccepted>(events_.front());
    ++cov_.accepted;
    cov_.accepted_while_full += was_full ? 1 : 0;
    if (accepted.id < generator_.next_fresh_id() - 1 && seen_.contains(accepted.id)) {
      ++cov_.reused_dead_id;
    }
    seen_.insert(accepted.id);
    const Price lo = profile_.config.min_price;
    const Price hi = profile_.config.max_price;
    cov_.band_edge_accepts += (accepted.price == lo || accepted.price == hi) ? 1 : 0;

    std::set<Price> prices;
    long trades = 0;
    for (const Event& e : events_) {
      if (const auto* t = std::get_if<TradeExecuted>(&e)) {
        ++trades;
        prices.insert(t->price);
        cov_.maker_partially_filled += t->maker_remaining > 0 ? 1 : 0;
        cov_.band_edge_trades += (t->price == lo || t->price == hi) ? 1 : 0;
      }
    }
    cov_.trades += trades;
    cov_.multi_maker_commands += trades >= 2 ? 1 : 0;
    cov_.multi_level_commands += prices.size() >= 2 ? 1 : 0;
    cov_.max_trades_in_command = std::max(cov_.max_trades_in_command, trades);
    cov_.max_levels_swept = std::max(cov_.max_levels_swept, static_cast<long>(prices.size()));
    if (trades > 0 && !engine_.book().best_price(opposite(accepted.side))) {
      ++cov_.side_emptied;
    }
    sample_best_levels();
    const bool rested = std::holds_alternative<OrderRested>(events_.back());
    cov_.taker_fully_filled += !rested ? 1 : 0;
    cov_.taker_partial_then_rested += (rested && trades > 0) ? 1 : 0;
    cov_.rested_without_trade += (rested && trades == 0) ? 1 : 0;
  }

  const WorkloadProfile& profile_;
  const RunOptions& options_;
  MatchingEngine engine_;
  ReferenceEngine ref_;
  EventAuditor auditor_;
  WorkloadGenerator generator_;
  Rng fault_rng_;
  std::vector<Event> events_;
  std::set<OrderId> seen_;
  Coverage cov_;
  RunReport report_;
  long step_ = 0;
  long fault_k_ = 0;
};

}  // namespace

RunReport run_differential(const WorkloadProfile& profile, const RunOptions& options) {
  if (options.fault_per_mille > 0 && options.injector == nullptr) {
    RunReport r;
    r.ok = false;
    r.failure = "fault_per_mille > 0 requires a FaultInjector";
    return r;
  }
  Runner runner{profile, options};
  return runner.run();
}

// -------------------------------------------------------------------- targets

std::vector<std::string> unmet_targets(const WorkloadProfile& p, const Coverage& c) {
  std::vector<std::string> unmet;
  auto need = [&](bool ok, const std::string& what) {
    if (!ok) {
      unmet.push_back(what);
    }
  };
  const long new_orders = c.commands - c.cancels - c.reject_count(RejectReason::UnknownOrderId);
  const auto capacity = static_cast<std::size_t>(p.config.max_live_orders);

  if (p.name == "mixed") {
    need(c.multi_level_commands > 0, "multi-level sweeps");
    need(c.accepted_while_full > 0, "orders accepted while the book is full");
    need(c.max_orders == capacity, "book reaches capacity");
    for (int r = 0; r < 8; ++r) {
      need(c.rejects[r] > 0, std::string("reject reason ") +
                                 std::string(to_string(static_cast<RejectReason>(r))));
    }
  } else if (p.name == "deep_book") {
    need(c.max_levels_per_side >= 1'000, "a side with >= 1000 price levels");
    need(c.max_orders >= capacity * 9 / 10, "book holds >= 90% of capacity (4500 orders)");
    need(c.max_levels_swept >= 20, "one command sweeping >= 20 price levels");
  } else if (p.name == "single_price") {
    need(c.max_level_orders >= 200, "a FIFO queue of >= 200 orders at the single price");
    need(c.band_edge_trades == c.trades, "every trade at the single price");
    need(c.max_trades_in_command >= 20, "one command filling >= 20 makers in one queue");
  } else if (p.name == "high_density") {
    // Calibration record: the first target, "trades >= accepted", sits at a
    // steady-state ceiling (each full maker fill removes an order some accepted
    // order rested; partial fills are at most one per taker) and was replaced
    // by the share of orders trading on arrival ("mixed": 38%). It was also
    // found to conflict with frequent side emptying, which needs small makers,
    // so that boundary moved to its own profile, thin_book.
    const long traded_on_arrival = c.taker_fully_filled + c.taker_partial_then_rested;
    need(traded_on_arrival * 10 >= c.accepted * 7, ">= 70% of accepted orders trade on arrival");
    need(c.maker_partially_filled * 2 >= c.trades, ">= 50% of trades leave the maker partially filled");
  } else if (p.name == "thin_book") {
    need(c.side_emptied * 20 >= c.commands, "a side traded empty in >= 5% of commands");
  } else if (p.name == "cancel_heavy") {
    need(c.cancels * 5 >= c.commands, "successful cancels >= 20% of commands");
    need(c.reject_count(RejectReason::UnknownOrderId) * 10 >= c.commands,
         "unknown-id cancels >= 10% of commands");
  } else if (p.name == "id_churn") {
    need(c.reject_count(RejectReason::DuplicateOrderId) * 20 >= new_orders,
         "duplicate-id rejections >= 5% of new orders");
    need(c.reused_dead_id * 20 >= c.accepted, "dead-id reuse >= 5% of accepted orders");
  } else if (p.name == "capacity_edge") {
    need(c.reject_count(RejectReason::BookFull) * 10 >= new_orders,
         "BOOK_FULL >= 10% of new orders");
    need(c.accepted_while_full * 100 >= c.commands, "accepted while full >= 1% of commands");
    need(c.max_orders == capacity, "book reaches capacity");
  } else if (p.name == "extreme_quantity") {
    need(c.max_level_quantity >= std::numeric_limits<Quantity>::max() / 4,
         "a level total >= INT64_MAX / 4");
    need(c.accepted_quantity > static_cast<Int128>(std::numeric_limits<std::int64_t>::max()),
         "cumulative accepted quantity > INT64_MAX (int128 accounting exercised)");
  } else if (p.name == "extreme_price") {
    need(c.band_edge_accepts * 100 >= c.accepted, "accepts at a band edge >= 1%");
    need(c.band_edge_trades > 0, "trades at a band edge");
  }
  return unmet;
}

}  // namespace lob::testkit
