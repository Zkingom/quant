// Session commit protocol (DESIGN.md §6.4): identity checks, restart,
// persistence failures, and the process-crash model.

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "io_test_util.hpp"
#include "lob/io/journal.hpp"
#include "lob/io/session.hpp"
#include "lob/testkit/rng.hpp"

namespace lob::io {
namespace {

using test::fresh;
using test::generate;

/// Fails the write that would take the total past `limit` bytes, after
/// writing the part that fits (a partial write, like a full disk or a crash
/// mid-write); optionally fails sync() from a given call on.
class FailingSink final : public ByteSink {
 public:
  explicit FailingSink(std::size_t limit, long fail_sync_from = -1)
      : limit_(limit), fail_sync_from_(fail_sync_from) {}
  void write(std::string_view bytes) override {
    if (data_.size() + bytes.size() > limit_) {
      data_.append(bytes.substr(0, limit_ - data_.size()));
      throw std::system_error(std::make_error_code(std::errc::no_space_on_device), "injected write failure");
    }
    data_.append(bytes);
  }
  void sync() override {
    if (fail_sync_from_ >= 0 && syncs_++ >= fail_sync_from_) {
      throw std::system_error(std::make_error_code(std::errc::io_error), "injected fsync failure");
    }
    synced_ = data_.size();
  }
  [[nodiscard]] const std::string& data() const { return data_; }
  [[nodiscard]] std::size_t synced_size() const { return synced_; }

 private:
  std::size_t limit_;
  long fail_sync_from_;
  long syncs_ = 0;
  std::string data_;
  std::size_t synced_ = 0;
};

const testkit::WorkloadProfile& mixed() { return *testkit::find_profile("mixed"); }

std::string reference_journal(const std::vector<SequencedCommand>& commands) {
  MemorySink sink;
  test::run_all(mixed().config, commands, sink);
  return sink.data();
}

TEST(Session, IdentityChecksNeverExecuteACommittedCommandTwice) {
  const auto commands = generate(mixed(), 1, 20);
  MemorySink sink;
  Session s{fresh(mixed().config), sink, SessionOptions{}};
  for (int i = 0; i < 10; ++i) {
    ASSERT_EQ(s.submit(commands[static_cast<std::size_t>(i)]).status, SubmitStatus::Committed);
  }
  const std::string before = sink.data();
  const SnapshotFile state = s.snapshot_file();

  const SubmitResult dup = s.submit(commands[4]);
  EXPECT_EQ(dup.status, SubmitStatus::Duplicate);
  const JournalBatch b5 = scan_journal(before).batches[4];
  EXPECT_EQ(dup.first_seq, b5.first_seq());  // the client can fetch the original result
  EXPECT_EQ(dup.last_seq, b5.last_seq());

  SequencedCommand altered = commands[4];
  altered.command = CancelOrder{.id = 424242};
  EXPECT_EQ(s.submit(altered).status, SubmitStatus::ContentMismatch);
  SequencedCommand gap = commands[12];
  EXPECT_EQ(s.submit(gap).status, SubmitStatus::SequenceGap);

  EXPECT_EQ(sink.data(), before);  // none of these touched the journal
  EXPECT_EQ(s.snapshot_file(), state);
  EXPECT_EQ(s.submit(commands[10]).status, SubmitStatus::Committed);
}

// Restart: recover from the journal written so far and re-submit the WHOLE
// command list. Committed commands are skipped as duplicates; the final
// journal equals the uninterrupted run byte for byte.
TEST(Session, RestartAfterAnyPrefixContinuesToTheIdenticalJournal) {
  const auto commands = generate(mixed(), 3, 600);
  const std::string reference = reference_journal(commands);
  for (const std::size_t k : {std::size_t{0}, std::size_t{1}, std::size_t{299}, std::size_t{599}, std::size_t{600}}) {
    MemorySink first;
    {
      Session s{fresh(mixed().config), first, SessionOptions{}};
      for (std::size_t i = 0; i < k; ++i) {
        ASSERT_EQ(s.submit(commands[i]).status, SubmitStatus::Committed);
      }
    }
    MemorySink second;
    second.write(first.data());
    Session resumed{recover(first.data(), std::nullopt, mixed().config), second, SessionOptions{}};
    std::size_t duplicates = 0;
    for (const auto& c : commands) {
      const auto r = resumed.submit(c);
      duplicates += r.status == SubmitStatus::Duplicate ? 1 : 0;
      ASSERT_TRUE(r.status == SubmitStatus::Committed || r.status == SubmitStatus::Duplicate);
    }
    EXPECT_EQ(duplicates, k);
    EXPECT_EQ(second.data(), reference) << "restart after " << k;
  }
}

// A write that fails part-way (sweeping the failure offset across the
// journal): the session poisons itself, acknowledges nothing, refuses
// everything; recovery from the bytes on "disk" gives exactly the last
// commit point; a new session continues to the same final journal.
TEST(Session, WriteFailureAtAnyOffsetPoisonsAndRecoversToTheLastCommit) {
  const auto commands = generate(mixed(), 4, 150);
  const std::string reference = reference_journal(commands);
  const JournalScan ref_scan = scan_journal(reference);
  testkit::Rng rng{11};
  for (int trial = 0; trial < 120; ++trial) {
    const auto limit = static_cast<std::size_t>(rng.uniform(0, static_cast<std::int64_t>(reference.size()) - 1));
    FailingSink sink{limit};
    std::size_t acknowledged = 0;
    bool poisoned = false;
    try {
      Session s{fresh(mixed().config), sink, SessionOptions{}};
      for (const auto& c : commands) {
        const auto r = s.submit(c);
        if (r.status == SubmitStatus::Poisoned) {
          poisoned = true;
          EXPECT_TRUE(s.poisoned());
          EXPECT_EQ(s.submit(c).status, SubmitStatus::Poisoned);  // refuses everything afterwards
          EXPECT_THROW((void)s.snapshot_file(), std::logic_error);  // state not exposed
          break;
        }
        ASSERT_EQ(r.status, SubmitStatus::Committed);
        ++acknowledged;
      }
    } catch (const std::system_error&) {
      poisoned = true;  // the header write itself failed (limit < header size)
    }
    ASSERT_TRUE(poisoned) << "limit " << limit;

    const Recovered rec = recover(sink.data(), std::nullopt, mixed().config);
    ASSERT_EQ(rec.last_cmd, acknowledged) << "limit " << limit;  // nothing unacknowledged survived
    const SnapshotFile expected = [&] {
      MemorySink tmp;
      std::vector<SequencedCommand> prefix(commands.begin(), commands.begin() + static_cast<std::ptrdiff_t>(acknowledged));
      return test::run_all(mixed().config, prefix, tmp);
    }();
    ASSERT_EQ(rec.state, expected.state);

    // Resume on the truncated journal (what FileSink(path, valid_bytes) does).
    MemorySink resumed_sink;
    resumed_sink.write(std::string_view(sink.data()).substr(0, rec.report.valid_bytes));
    Session resumed{rec, resumed_sink, SessionOptions{}};
    for (const auto& c : commands) {
      ASSERT_NE(resumed.submit(c).status, SubmitStatus::Poisoned);
    }
    ASSERT_EQ(resumed_sink.data(), reference) << "limit " << limit;
  }
  EXPECT_EQ(ref_scan.batches.size(), commands.size());
}

// fsync failure in durable mode: the batch bytes were written but the commit
// was not confirmed, so the command is not acknowledged and the session is
// poisoned. If those bytes do reach the disk, recovery treats the batch as
// committed and a client retry gets DUPLICATE (DESIGN Q2: "written, not
// acknowledged").
TEST(Session, SyncFailureIsNotAcknowledgedButMayBeCommitted) {
  const auto commands = generate(mixed(), 6, 30);
  FailingSink sink{std::string::npos, /*fail_sync_from=*/11};  // header sync + 10 batches succeed
  Session s{fresh(mixed().config), sink, SessionOptions{.durable = true, .prepare_retries = 2}};
  std::size_t acked = 0;
  for (const auto& c : commands) {
    if (s.submit(c).status != SubmitStatus::Committed) {
      break;
    }
    ++acked;
  }
  EXPECT_EQ(acked, 10U);
  EXPECT_TRUE(s.poisoned());
  const Recovered rec = recover(sink.data(), std::nullopt, mixed().config);
  EXPECT_EQ(rec.last_cmd, 11U);  // the 11th batch was written completely
  MemorySink again;
  again.write(sink.data());
  Session resumed{rec, again, SessionOptions{}};
  EXPECT_EQ(resumed.submit(commands[10]).status, SubmitStatus::Duplicate);
}

// Process-crash model: a crash keeps an arbitrary prefix of the written bytes
// that is at least as long as what was synced. In durable mode every
// acknowledged batch was synced before the acknowledgement, so recovery
// always contains every acknowledged command.
TEST(Session, DurableModeNeverLosesAnAcknowledgedCommandInTheCrashModel) {
  const auto commands = generate(mixed(), 8, 400);
  MemorySink sink;
  Session s{fresh(mixed().config), sink, SessionOptions{.durable = true, .prepare_retries = 2}};
  testkit::Rng rng{5};
  for (std::size_t i = 0; i < commands.size(); ++i) {
    ASSERT_EQ(s.submit(commands[i]).status, SubmitStatus::Committed);
    const std::size_t acked = i + 1;
    if (rng.percent(10)) {
      // Crash now: anything between the synced size and the written size.
      const auto keep = static_cast<std::size_t>(rng.uniform(static_cast<std::int64_t>(sink.synced_size()),
                                                             static_cast<std::int64_t>(sink.data().size())));
      const Recovered rec = recover(std::string_view(sink.data()).substr(0, keep), std::nullopt, mixed().config);
      ASSERT_GE(rec.last_cmd, acked) << "acknowledged command lost";
    }
  }
}

// The same model in buffered (non-durable) mode demonstrates the documented
// limitation: acknowledged commands CAN be lost (DESIGN §6.6).
TEST(Session, BufferedModeCanLoseAcknowledgedCommandsInTheCrashModel) {
  const auto commands = generate(mixed(), 8, 50);
  MemorySink sink;
  Session s{fresh(mixed().config), sink, SessionOptions{.durable = false, .prepare_retries = 2}};
  for (const auto& c : commands) {
    ASSERT_EQ(s.submit(c).status, SubmitStatus::Committed);
  }
  EXPECT_EQ(sink.synced_size(), 0U);  // nothing synced yet
  const Recovered rec = recover(std::string_view(sink.data()).substr(0, sink.synced_size()), std::nullopt,
                                mixed().config);
  EXPECT_EQ(rec.last_cmd, 0U);  // a crash here loses all 50 acknowledged commands
  s.sync();
  EXPECT_EQ(sink.synced_size(), sink.data().size());
}

}  // namespace
}  // namespace lob::io
