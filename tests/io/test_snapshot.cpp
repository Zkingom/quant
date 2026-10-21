// Engine snapshot / restore and the snapshot file format (DESIGN.md §6.5).

#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "../engine/engine_test_util.hpp"
#include "io_test_util.hpp"
#include "lob/engine/matching_engine.hpp"
#include "lob/io/snapshot_file.hpp"
#include "lob/testkit/describe.hpp"

namespace lob::io {
namespace {

constexpr EngineConfig kConfig{.min_price = 1, .max_price = 100, .max_order_quantity = 50, .max_live_orders = 6};

EngineSnapshot base() {
  return EngineSnapshot{.config = kConfig,
                        .last_seq = 20,
                        .orders = {{.id = 1, .side = Side::Buy, .price = 50, .remaining = 5, .seq = 3},
                                   {.id = 2, .side = Side::Buy, .price = 50, .remaining = 4, .seq = 7},
                                   {.id = 3, .side = Side::Buy, .price = 49, .remaining = 1, .seq = 2},
                                   {.id = 4, .side = Side::Sell, .price = 52, .remaining = 9, .seq = 9},
                                   {.id = 5, .side = Side::Sell, .price = 53, .remaining = 2, .seq = 1}}};
}

TEST(EngineSnapshot, RestoreThenSnapshotIsTheIdentity) {
  const EngineSnapshot s = base();
  ASSERT_EQ(find_snapshot_violation(s), std::nullopt);
  const MatchingEngine e = MatchingEngine::restore(s);
  EXPECT_EQ(e.snapshot(), s);
  EXPECT_EQ(e.last_seq(), 20U);
  EXPECT_EQ(e.find_invariant_violation(), std::nullopt);
  // FIFO restored by seq: order 1 (seq 3) is ahead of order 2 (seq 7).
  EXPECT_EQ(e.book().front(Side::Buy)->id, 1U);
}

TEST(EngineSnapshot, RestoredEngineContinuesWithTheNextSeqAndPriority) {
  MatchingEngine e = MatchingEngine::restore(base());
  std::vector<Event> events;
  e.process(NewOrder{.id = 9, .side = Side::Sell, .price = 50, .quantity = 7}, events);
  ASSERT_EQ(events.size(), 3U);
  EXPECT_EQ(seq_of(events[0]), 21U);  // continues after last_seq
  EXPECT_EQ(std::get<TradeExecuted>(events[1]).maker_id, 1U);  // FIFO preserved
  EXPECT_EQ(std::get<TradeExecuted>(events[2]).maker_id, 2U);
}

TEST(EngineSnapshot, EveryViolationIsReported) {
  struct Case {
    std::string name;
    std::function<void(EngineSnapshot&)> damage;
    std::string needle;
  };
  const std::vector<Case> cases{
      {"config", [](EngineSnapshot& s) { s.config.min_price = 0; }, "config:"},
      {"capacity", [](EngineSnapshot& s) { s.config.max_live_orders = 4; }, "more orders than max_live_orders"},
      {"id 0", [](EngineSnapshot& s) { s.orders[0].id = 0; }, "INVALID_ORDER_ID"},
      {"side", [](EngineSnapshot& s) { s.orders[0].side = static_cast<Side>(7); }, "INVALID_SIDE"},
      {"band", [](EngineSnapshot& s) { s.orders[3].price = 101; s.orders[4].price = 102; }, "PRICE_OUT_OF_RANGE"},
      {"remaining 0", [](EngineSnapshot& s) { s.orders[1].remaining = 0; }, "INVALID_QUANTITY"},
      {"remaining > max", [](EngineSnapshot& s) { s.orders[1].remaining = 51; }, "QUANTITY_TOO_LARGE"},
      {"seq 0", [](EngineSnapshot& s) { s.orders[2].seq = 0; }, "outside [1, last_seq"},
      {"seq > last", [](EngineSnapshot& s) { s.orders[2].seq = 21; }, "outside [1, last_seq"},
      {"dup id", [](EngineSnapshot& s) { s.orders[4].id = 1; }, "duplicate id"},
      {"dup seq", [](EngineSnapshot& s) { s.orders[4].seq = 3; }, "duplicate seq"},
      {"bid after ask", [](EngineSnapshot& s) { std::swap(s.orders[2], s.orders[3]); }, "not canonical"},
      {"price order", [](EngineSnapshot& s) { std::swap(s.orders[1], s.orders[2]); }, "not canonical"},
      {"fifo order", [](EngineSnapshot& s) { std::swap(s.orders[0], s.orders[1]); }, "FIFO (seq) order not canonical"},
      {"crossed", [](EngineSnapshot& s) { s.orders[3].price = 50; s.orders[4].price = 51; }, "crossed or locked"},
  };
  for (const Case& c : cases) {
    EngineSnapshot s = base();
    c.damage(s);
    const auto v = find_snapshot_violation(s);
    ASSERT_TRUE(v.has_value()) << c.name;
    EXPECT_NE(v->find(c.needle), std::string::npos) << c.name << ": " << *v;
    EXPECT_THROW((void)MatchingEngine::restore(s), std::invalid_argument) << c.name;
  }
}

TEST(EngineSnapshot, SeqExhaustionIsReachableThroughRestore) {
  EngineSnapshot s = base();
  s.last_seq = std::numeric_limits<SeqNum>::max() - 1;
  MatchingEngine e = MatchingEngine::restore(s);
  std::vector<Event> events;
  // A resting order needs 2 seqs; one is left.
  EXPECT_THROW(e.process(NewOrder{.id = 8, .side = Side::Buy, .price = 10, .quantity = 1}, events),
               std::overflow_error);
  EXPECT_TRUE(events.empty());
  e.process(CancelOrder{.id = 77}, events);  // a rejection needs exactly one
  EXPECT_EQ(seq_of(events.back()), std::numeric_limits<SeqNum>::max());
}

// The core property: an engine restored from a snapshot taken after k
// commands (through the text format) produces exactly the same events for
// commands k+1..n, and the same final state, as the engine that never
// stopped. All profiles, several stream lengths and cut points.
TEST(EngineSnapshot, RestoredEngineBehavesIdenticallyAcrossProfilesLengthsAndCutPoints) {
  long checked_commands = 0;
  for (const auto& profile : testkit::standard_profiles()) {
    for (const std::size_t n : {std::size_t{40}, std::size_t{700}, std::size_t{3'000}}) {
      const auto commands = test::generate(profile, 17 + n, n);
      for (const std::size_t k : {std::size_t{0}, n / 3, n / 2, n - 1}) {
        SCOPED_TRACE(profile.name + " n=" + std::to_string(n) + " k=" + std::to_string(k));
        MatchingEngine original{profile.config};
        std::vector<Event> ev;
        for (std::size_t i = 0; i < k; ++i) {
          original.process(commands[i].command, ev);
        }
        const std::string text = encode_snapshot(SnapshotFile{.state = original.snapshot(), .last_cmd = k});
        MatchingEngine restored = MatchingEngine::restore(parse_snapshot(text).state);
        std::vector<Event> a;
        std::vector<Event> b;
        for (std::size_t i = k; i < n; ++i) {
          a.clear();
          b.clear();
          original.process(commands[i].command, a);
          restored.process(commands[i].command, b);
          ASSERT_EQ(a, b) << "command " << i + 1 << '\n' << testkit::describe(a) << testkit::describe(b);
          ++checked_commands;
        }
        ASSERT_EQ(original.snapshot(), restored.snapshot());
        ASSERT_EQ(restored.find_invariant_violation(), std::nullopt);
      }
    }
  }
  std::printf("[snapshot] restored engines matched the originals on %ld subsequent commands\n",
              checked_commands);
}

TEST(SnapshotFile, RoundTripAndHash) {
  const SnapshotFile f{.state = base(), .last_cmd = 12};
  const std::string text = encode_snapshot(f);
  EXPECT_EQ(parse_snapshot(text), f);
  EXPECT_EQ(state_hash(parse_snapshot(text).state), state_hash(f.state));
  SnapshotFile g = f;
  g.state.orders[0].remaining = 4;
  EXPECT_NE(state_hash(g.state), state_hash(f.state));
  g = f;
  g.last_cmd = 13;  // the watermark is not part of the state
  EXPECT_EQ(state_hash(g.state), state_hash(f.state));
}

TEST(SnapshotFile, DamageIsDetected) {
  const std::string text = encode_snapshot(SnapshotFile{.state = base(), .last_cmd = 12});
  struct Case {
    std::string bytes;
    std::string needle;
  };
  std::string flipped = text;
  flipped[text.find("ORDER BUY 50 1 5") + 15] = '6';  // remaining 5 -> 6
  const std::size_t orders_line = text.find("ORDERS 5");
  std::string recount = text;
  recount[orders_line + 7] = '4';
  const std::vector<Case> cases{
      {flipped, "checksum mismatch"},
      {recount, "expected END"},
      {text.substr(0, text.size() - 10), "snapshot truncated"},
      {text + "extra\n", "unexpected bytes after END"},
      {"LOB-SNAPSHOT v9\n" + text.substr(16), "unsupported header"},
  };
  for (const Case& c : cases) {
    try {
      (void)parse_snapshot(c.bytes);
      ADD_FAILURE() << "accepted: " << c.needle;
    } catch (const ParseError& e) {
      EXPECT_NE(std::string(e.what()).find(c.needle), std::string::npos) << e.what();
    }
  }
  // Valid checksum but an invalid state (re-encoded after damage): rejected.
  SnapshotFile crossed{.state = base(), .last_cmd = 1};
  crossed.state.orders[3].price = 50;
  crossed.state.orders[4].price = 51;
  EXPECT_THROW((void)parse_snapshot(encode_snapshot(crossed)), ParseError);
}

}  // namespace
}  // namespace lob::io
