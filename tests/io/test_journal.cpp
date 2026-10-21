// Journal framing and tail classification (DESIGN.md §6.2 Q4/Q6).

#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <vector>

#include "io_test_util.hpp"
#include "lob/io/journal.hpp"
#include "lob/io/recovery.hpp"

namespace lob::io {
namespace {

constexpr EngineConfig kConfig{.min_price = 1, .max_price = 100, .max_order_quantity = 50, .max_live_orders = 20};

std::vector<SequencedCommand> sample_commands() {
  using lob::NewOrder;
  return {
      {.cmd_seq = 1, .command = NewOrder{.id = 1, .side = Side::Sell, .price = 50, .quantity = 5}},
      {.cmd_seq = 2, .command = NewOrder{.id = 2, .side = Side::Sell, .price = 51, .quantity = 5}},
      {.cmd_seq = 3, .command = NewOrder{.id = 3, .side = Side::Buy, .price = 51, .quantity = 8}},  // 2 trades
      {.cmd_seq = 4, .command = CancelOrder{.id = 2}},
      {.cmd_seq = 5, .command = NewOrder{.id = 4, .side = Side::Buy, .price = 0, .quantity = 1}},  // rejected
  };
}

std::string sample_journal() {
  MemorySink sink;
  test::run_all(kConfig, sample_commands(), sink);
  return sink.data();
}

TEST(Journal, EncodeScanRoundTrip) {
  const std::string bytes = sample_journal();
  const JournalScan scan = scan_journal(bytes);
  ASSERT_TRUE(scan.config.has_value());
  EXPECT_EQ(*scan.config, kConfig);
  ASSERT_EQ(scan.batches.size(), 5U);
  EXPECT_EQ(scan.tail, TailState::Clean);
  EXPECT_EQ(scan.valid_bytes, bytes.size());
  EXPECT_EQ(scan.batches[2].events.size(), 3U);  // ACCEPTED, TRADE, TRADE
  EXPECT_EQ(scan.batches[2].first_seq(), 5U);
  EXPECT_EQ(scan.batches[4].command, sample_commands()[4]);
  // Re-encoding every scanned batch reproduces the file byte for byte.
  std::string again = encode_journal_header(kConfig);
  for (const JournalBatch& b : scan.batches) {
    again += encode_batch(b.command, b.events);
  }
  EXPECT_EQ(again, bytes);
}

TEST(Journal, EmptyAndTornHeaderCountAsEmptyJournal) {
  EXPECT_FALSE(scan_journal("").config.has_value());
  const std::string header = encode_journal_header(kConfig);
  for (std::size_t cut = 1; cut < header.size(); ++cut) {
    const JournalScan s = scan_journal(std::string_view(header).substr(0, cut));
    EXPECT_FALSE(s.config.has_value()) << cut;
    EXPECT_EQ(s.tail, TailState::Torn) << cut;
    EXPECT_EQ(s.valid_bytes, 0U) << cut;
  }
}

// Truncation at EVERY byte offset (the process-crash model): the scan keeps
// exactly the batches that are completely contained in the prefix.
TEST(Journal, TruncationAtEveryOffsetKeepsExactlyTheCompleteBatches) {
  const std::string bytes = sample_journal();
  const JournalScan full = scan_journal(bytes);
  const std::size_t header = encode_journal_header(kConfig).size();
  for (std::size_t cut = header; cut <= bytes.size(); ++cut) {
    const JournalScan s = scan_journal(std::string_view(bytes).substr(0, cut));
    std::size_t complete = 0;
    while (complete < full.batches.size() && full.batches[complete].end_offset <= cut) {
      ++complete;
    }
    ASSERT_EQ(s.batches.size(), complete) << "cut " << cut;
    const std::size_t valid = complete == 0 ? header : full.batches[complete - 1].end_offset;
    EXPECT_EQ(s.valid_bytes, valid) << "cut " << cut;
    EXPECT_EQ(s.dropped_bytes, cut - valid) << "cut " << cut;
    EXPECT_EQ(s.tail, cut == valid ? TailState::Clean : TailState::Torn) << "cut " << cut;
  }
}

TEST(Journal, NulPaddedTailIsTorn) {
  const std::string bytes = sample_journal();
  const JournalScan full = scan_journal(bytes);
  const std::size_t cut = full.batches[3].end_offset + 7;  // into batch 5
  const std::string padded = bytes.substr(0, cut) + std::string(512, '\0');
  const JournalScan s = scan_journal(padded);
  EXPECT_EQ(s.batches.size(), 4U);
  EXPECT_EQ(s.tail, TailState::Torn);
  EXPECT_EQ(s.valid_bytes, full.batches[3].end_offset);
}

// Every single-byte modification of the LAST batch is either reported as
// corruption or makes the scan drop that whole batch; a modified batch is
// never accepted.
TEST(Journal, NoSingleByteModificationOfTheLastBatchIsAccepted) {
  const std::string bytes = sample_journal();
  const JournalScan full = scan_journal(bytes);
  const JournalBatch& last = full.batches.back();
  long corrupt = 0;
  long dropped = 0;
  for (std::size_t i = last.begin_offset; i < last.end_offset; ++i) {
    for (const char replacement : {'X', '0', ' ', '\n', '\0'}) {
      if (bytes[i] == replacement) {
        continue;
      }
      std::string damaged = bytes;
      damaged[i] = replacement;
      try {
        const JournalScan s = scan_journal(damaged);
        ASSERT_EQ(s.batches.size(), full.batches.size() - 1)
            << "byte " << i << " -> " << int(replacement) << " was accepted";
        ASSERT_EQ(s.tail, TailState::Torn);
        ++dropped;
      } catch (const JournalCorruption&) {
        ++corrupt;
      }
    }
  }
  std::printf("[journal] last-batch single-byte damage: %ld reported corrupt, %ld dropped as torn\n",
              corrupt, dropped);
  EXPECT_GT(corrupt, 0);
}

TEST(Journal, DamageBeforeTheTailIsCorruption) {
  const std::string bytes = sample_journal();
  const JournalScan full = scan_journal(bytes);
  const JournalBatch& b = full.batches[1];
  std::string damaged = bytes;
  damaged[b.begin_offset + 8] = '9';  // inside batch 2, batches 3..5 follow
  EXPECT_THROW((void)scan_journal(damaged), JournalCorruption);
  // A missing middle batch (cut out cleanly) breaks cmd_seq contiguity.
  const std::string missing = bytes.substr(0, b.begin_offset) + bytes.substr(b.end_offset);
  try {
    (void)scan_journal(missing);
    ADD_FAILURE() << "accepted a journal with a batch removed";
  } catch (const JournalCorruption& e) {
    EXPECT_NE(std::string(e.what()).find("cmd_seq 3 out of order (expected 2)"), std::string::npos) << e.what();
  }
}

TEST(Journal, FramingAndSequenceViolationsAreCorruption) {
  const std::string header = encode_journal_header(kConfig);
  const SequencedCommand c{.cmd_seq = 1, .command = CancelOrder{.id = 9}};
  const Event rejected = OrderRejected{.seq = 1, .command = CommandType::CancelOrder, .id = 9,
                                       .reason = RejectReason::UnknownOrderId};
  struct Case {
    std::string bytes;
    std::string needle;
  };
  std::string good = encode_batch(c, std::vector<Event>{rejected});
  std::string bad_sum = good;
  bad_sum[bad_sum.size() - 2] = bad_sum[bad_sum.size() - 2] == 'a' ? 'b' : 'a';
  const std::vector<Case> cases{
      {header + bad_sum, "checksum mismatch"},
      {header + encode_batch(SequencedCommand{.cmd_seq = 2, .command = c.command}, std::vector<Event>{
                                 OrderRejected{.seq = 1, .command = CommandType::CancelOrder, .id = 9,
                                               .reason = RejectReason::UnknownOrderId}}),
       "cmd_seq 2 out of order (expected 1)"},
      {header + encode_batch(c, std::vector<Event>{OrderRejected{.seq = 5, .command = CommandType::CancelOrder,
                                                                 .id = 9, .reason = RejectReason::UnknownOrderId}}),
       "event seq 5 out of order (expected 1)"},
      {header + "BATCH 1 2 CANCEL 1 9\n" + format_event(rejected) + "\nCOMMIT 1 0000000000000000\n",
       "COMMIT after 1 of 2 events"},
      {header + "HELLO\n", "expected BATCH line"},
      {"LOB-JOURNAL v2\n" + header.substr(15), "unsupported header"},
  };
  for (const Case& k : cases) {
    try {
      (void)scan_journal(k.bytes);
      ADD_FAILURE() << "accepted: " << k.needle;
    } catch (const JournalCorruption& e) {
      EXPECT_NE(std::string(e.what()).find(k.needle), std::string::npos) << e.what();
    }
  }
}

}  // namespace
}  // namespace lob::io
