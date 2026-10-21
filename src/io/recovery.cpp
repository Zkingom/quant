#include "lob/io/recovery.hpp"

#include <algorithm>
#include <variant>

#include "lob/io/checksum.hpp"

namespace lob::io {

std::uint64_t command_digest(const SequencedCommand& command) {
  return fnv1a64(format_command(command));
}

// ------------------------------------------------------------------ EventFold

EventFold::EventFold(const EngineSnapshot& start) : config_(start.config), last_seq_(start.last_seq) {
  for (const SnapshotOrder& o : start.orders) {
    resting_.emplace(o.id, Order{o.side, o.price, o.remaining, o.seq});
  }
}

namespace {

bool crosses(Side taker_side, Price limit, Price maker_price) {
  return taker_side == Side::Buy ? maker_price <= limit : maker_price >= limit;
}

}  // namespace

void EventFold::apply(const JournalBatch& batch) {
  auto corrupt = [&](const std::string& message) {
    throw JournalCorruption(batch.begin_offset, batch.begin_line,
                            "batch cmd_seq " + std::to_string(batch.command.cmd_seq) + ": " + message);
  };
  const std::vector<Event>& ev = batch.events;
  const std::size_t n = ev.size();

  if (const auto* cancel = std::get_if<CancelOrder>(&batch.command.command)) {
    if (n != 1) {
      corrupt("a cancel produces exactly one event");
    }
    if (const auto* r = std::get_if<OrderRejected>(&ev[0])) {
      if (r->command != CommandType::CancelOrder || r->id != cancel->id) {
        corrupt("REJECTED does not match the cancel");
      }
    } else if (const auto* c = std::get_if<OrderCancelled>(&ev[0])) {
      const auto it = resting_.find(cancel->id);
      if (c->reason != CancelReason::Requested || c->id != cancel->id || it == resting_.end() ||
          it->second.side != c->side || it->second.price != c->price ||
          it->second.remaining != c->remaining) {
        corrupt("CANCELLED does not match a resting order");
      }
      resting_.erase(it);
    } else {
      corrupt("a cancel produces CANCELLED or REJECTED");
    }
    last_seq_ = batch.last_seq();
    return;
  }

  const NewOrder& o = std::get<NewOrder>(batch.command.command);
  if (const auto* r = std::get_if<OrderRejected>(&ev[0])) {
    if (n != 1 || r->command != CommandType::NewOrder || r->id != o.id) {
      corrupt("REJECTED does not match the new order");
    }
    last_seq_ = batch.last_seq();
    return;
  }
  const auto* a = std::get_if<OrderAccepted>(&ev[0]);
  if (a == nullptr || a->id != o.id || a->side != o.side || a->price != o.price ||
      a->quantity != o.quantity) {
    corrupt("first event must be the ACCEPTED of this order");
  }
  if (resting_.contains(o.id)) {
    corrupt("ACCEPTED an id that is resting");
  }
  Quantity remaining = o.quantity;
  std::size_t i = 1;
  for (; i < n && std::holds_alternative<TradeExecuted>(ev[i]); ++i) {
    const auto& t = std::get<TradeExecuted>(ev[i]);
    const auto maker = resting_.find(t.maker_id);
    if (t.taker_id != o.id || t.taker_side != o.side || maker == resting_.end() ||
        maker->second.side == o.side || maker->second.price != t.price ||
        !crosses(o.side, o.price, t.price) || t.quantity <= 0 || t.quantity > remaining ||
        t.quantity > maker->second.remaining) {
      corrupt("TRADE inconsistent with the order or the resting book");
    }
    remaining -= t.quantity;
    maker->second.remaining -= t.quantity;
    if (t.taker_remaining != remaining || t.maker_remaining != maker->second.remaining) {
      corrupt("TRADE remaining quantities inconsistent");
    }
    if (maker->second.remaining == 0) {
      resting_.erase(maker);
    }
  }
  if (remaining == 0) {
    if (i != n) {
      corrupt("events after the order was completely filled");
    }
  } else {
    if (i != n - 1) {
      corrupt("the remainder must end with exactly one RESTED or CANCELLED");
    }
    if (const auto* rested = std::get_if<OrderRested>(&ev[i])) {
      if (rested->id != o.id || rested->side != o.side || rested->price != o.price ||
          rested->remaining != remaining) {
        corrupt("RESTED does not match the remainder");
      }
      resting_.emplace(o.id, Order{o.side, o.price, remaining, a->seq});  // priority = accepted seq
    } else if (const auto* c = std::get_if<OrderCancelled>(&ev[i]);
               c != nullptr && c->reason == CancelReason::InsufficientResources) {
      if (c->id != o.id || c->side != o.side || c->price != o.price || c->remaining != remaining) {
        corrupt("CANCELLED(INSUFFICIENT_RESOURCES) does not match the remainder");
      }
    } else {
      corrupt("the remainder must end with RESTED or CANCELLED(INSUFFICIENT_RESOURCES)");
    }
  }
  last_seq_ = batch.last_seq();
}

EngineSnapshot EventFold::snapshot() const {
  EngineSnapshot s{.config = config_, .last_seq = last_seq_, .orders = {}};
  for (const auto& [id, o] : resting_) {
    s.orders.push_back(SnapshotOrder{.id = id, .side = o.side, .price = o.price, .remaining = o.remaining, .seq = o.seq});
  }
  // Canonical: bids best first, then asks best first; FIFO (seq) within a price.
  std::sort(s.orders.begin(), s.orders.end(), [](const SnapshotOrder& x, const SnapshotOrder& y) {
    if (x.side != y.side) {
      return x.side == Side::Buy;
    }
    if (x.price != y.price) {
      return x.side == Side::Buy ? x.price > y.price : x.price < y.price;
    }
    return x.seq < y.seq;
  });
  return s;
}

// ------------------------------------------------------------------- recover

Recovered recover(std::string_view journal_bytes, const std::optional<SnapshotFile>& snapshot,
                  const std::optional<EngineConfig>& expected_config) {
  const JournalScan scan = scan_journal(journal_bytes);

  Recovered out;
  RecoveryReport& rep = out.report;
  rep.journal_has_header = scan.config.has_value();
  rep.batches_in_journal = scan.batches.size();
  rep.tail = scan.tail;
  rep.valid_bytes = scan.valid_bytes;
  rep.dropped_bytes = scan.dropped_bytes;

  // One config for the whole history.
  std::optional<EngineConfig> config = scan.config;
  auto agree = [&](const EngineConfig& other, const char* what) {
    if (!config) {
      config = other;
    } else if (!(*config == other)) {
      throw RecoveryError(std::string("config mismatch: ") + what + " has " + format_config(other) +
                          ", journal/other has " + format_config(*config));
    }
  };
  if (snapshot) {
    agree(snapshot->state.config, "snapshot");
  }
  if (expected_config) {
    agree(*expected_config, "expected (command file)");
  }
  if (!config) {
    throw RecoveryError("no config: empty journal, no snapshot and no expected config");
  }

  // Starting point and its boundary with the journal.
  EngineSnapshot start{.config = *config, .last_seq = 0, .orders = {}};
  CmdSeq start_cmd = 0;
  if (snapshot && snapshot->last_cmd > 0) {
    const CmdSeq k = snapshot->last_cmd;
    if (k > scan.batches.size()) {
      throw RecoveryError("snapshot is at cmd_seq " + std::to_string(k) + " but the journal only has " +
                          std::to_string(scan.batches.size()) +
                          " committed batches (journal lost committed history, or another history)");
    }
    const JournalBatch& boundary = scan.batches[k - 1];
    if (boundary.last_seq() != snapshot->state.last_seq) {
      throw RecoveryError("snapshot LAST_SEQ " + std::to_string(snapshot->state.last_seq) +
                          " != journal batch " + std::to_string(k) + " last seq " +
                          std::to_string(boundary.last_seq()) + " (different histories)");
    }
    start = snapshot->state;
    start_cmd = k;
    rep.snapshot_used = true;
    rep.snapshot_cmd = k;
  } else if (snapshot && snapshot->state.last_seq != 0) {
    throw RecoveryError("snapshot at cmd_seq 0 must have LAST_SEQ 0");
  }

  EventFold fold{start};
  out.committed.reserve(scan.batches.size());
  for (const JournalBatch& b : scan.batches) {
    const auto* last_cancel = std::get_if<OrderCancelled>(&b.events.back());
    out.committed.push_back(CommittedCommand{
        .digest = command_digest(b.command),
        .first_seq = b.first_seq(),
        .last_seq = b.last_seq(),
        .resource_failure = last_cancel != nullptr && last_cancel->reason == CancelReason::InsufficientResources});
    if (b.command.cmd_seq > start_cmd) {
      fold.apply(b);
      ++rep.batches_applied;
    }
  }
  out.state = fold.snapshot();
  out.last_cmd = scan.batches.empty() ? start_cmd : scan.batches.back().command.cmd_seq;
  if (const auto violation = find_snapshot_violation(out.state)) {
    throw RecoveryError("recovered state is invalid: " + *violation);
  }
  return out;
}

}  // namespace lob::io
