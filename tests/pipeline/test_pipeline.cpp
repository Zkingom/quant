// L3' stage 2: decoder thread -> SPSC -> book thread must reproduce the
// single-threaded replay exactly (event stream bytes and final book hash).
// Label "concurrency": also runs under gcc-tsan / clang-tsan.

#include "lob/pipeline/pipeline.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "lob/pipeline/async_logger.hpp"
#include "lob/testkit/itch_generator.hpp"
#include "lob/testkit/itch_samples.hpp"

namespace {

namespace pl = lob::pipeline;
namespace md = lob::md;
namespace tk = lob::testkit::itch;

md::FeedConfig target_config() {
  md::FeedConfig c;
  c.symbol = md::make_symbol(tk::symbol_of(1));
  c.book = md::BookConfig{4096, 1024};
  return c;
}

struct Outcome {
  std::string text;
  std::uint64_t event_hash{};
  std::uint64_t book_hash{};
  std::uint64_t frames{};
  md::FeedState state{};
  md::FeedCounters counters{};
};

Outcome run(const tk::Buf& stream, bool pipelined, std::size_t queue_cap, std::size_t max_read,
            const md::FeedConfig& cfg, std::uint64_t pace = 0) {
  pl::EventDigest digest;
  digest.keep_text = true;
  pl::BookStage<pl::EventDigest> stage{cfg, digest};
  pl::MemorySource src{stream, max_read};
  pl::RunConfig rc;
  rc.chunk_bytes = 1U << 17;
  rc.queue_capacity = queue_cap;
  rc.pace_ticks_per_msg = pace;
  const pl::RunStats st = pipelined ? pl::run_pipelined(src, stage, rc) : pl::run_single(src, stage, rc);
  Outcome o;
  o.text = std::move(digest.text);
  o.event_hash = digest.hash;
  o.book_hash = stage.feed().book().state_hash();
  o.frames = st.frames;
  o.state = stage.feed().state();
  o.counters = stage.feed().counters();
  return o;
}

void expect_same(const Outcome& a, const Outcome& b, const std::string& what) {
  EXPECT_EQ(a.text, b.text) << what;
  EXPECT_EQ(a.event_hash, b.event_hash) << what;
  EXPECT_EQ(a.book_hash, b.book_hash) << what;
  EXPECT_EQ(a.frames, b.frames) << what;
  EXPECT_EQ(a.state, b.state) << what;
  EXPECT_EQ(a.counters.applied, b.counters.applied) << what;
  EXPECT_EQ(a.counters.other_stock, b.counters.other_stock) << what;
  EXPECT_EQ(a.counters.skipped, b.counters.skipped) << what;
}

#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
constexpr std::size_t kMessages = 8000;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
constexpr std::size_t kMessages = 8000;
#else
constexpr std::size_t kMessages = 40000;
#endif
#else
constexpr std::size_t kMessages = 40000;
#endif

TEST(Pipeline, PipelinedEqualsSingleThreadAcrossSeedsQueuesAndChunks) {
  for (std::uint64_t seed = 1; seed <= 3; ++seed) {
    tk::GeneratorParams gp;
    gp.seed = seed;
    gp.messages = kMessages;
    const tk::Buf stream = tk::generate_stream(gp);
    const Outcome ref = run(stream, false, 1024, SIZE_MAX, target_config());
    ASSERT_EQ(ref.state, md::FeedState::Live) << "generator must produce a valid stream";
    ASSERT_GT(ref.counters.applied, kMessages / 8);
    for (const std::size_t cap : {1U, 2U, 64U, 16384U}) {
      for (const std::size_t max_read : {SIZE_MAX, std::size_t{4093}}) {
        expect_same(ref, run(stream, true, cap, max_read, target_config()),
                    "seed " + std::to_string(seed) + " cap " + std::to_string(cap) + " read " +
                        std::to_string(max_read));
      }
    }
    // single-threaded with tiny reads must also agree (chunk carry-over)
    expect_same(ref, run(stream, false, 1, 977, target_config()), "single, small reads");
  }
}

// The same stream gives the same hashes on every compiler and build type:
// the constants are checked in all six presets (GCC and Clang).
TEST(Pipeline, HashesArePinnedAcrossCompilers) {
  tk::GeneratorParams gp;
  gp.seed = 42;
  gp.messages = 5000;
  const Outcome o = run(tk::generate_stream(gp), true, 64, SIZE_MAX, target_config());
  EXPECT_EQ(o.frames, 5000U);
  EXPECT_EQ(o.event_hash, 0x38ab7dce042ca73aULL) << std::hex << "event hash 0x" << o.event_hash;
  EXPECT_EQ(o.book_hash, 0xbb6a53fbcefa0f27ULL) << std::hex << "book hash 0x" << o.book_hash;
}

TEST(Pipeline, FaultyStreamsAlsoAgree) {
  tk::GeneratorParams gp;
  gp.seed = 9;
  gp.messages = 3000;
  tk::Buf stream = tk::generate_stream(gp);
  // Truncated tail.
  tk::Buf truncated(stream.begin(), stream.end() - 5);
  const Outcome a = run(truncated, false, 8, SIZE_MAX, target_config());
  EXPECT_EQ(a.state, md::FeedState::Stale);
  expect_same(a, run(truncated, true, 8, 1000, target_config()), "truncated");
  // Corrupted middle byte (somewhere in a frame).
  tk::Buf corrupt = stream;
  corrupt[corrupt.size() / 2] ^= 0x5a;
  const Outcome b = run(corrupt, false, 8, SIZE_MAX, target_config());
  expect_same(b, run(corrupt, true, 8, 1000, target_config()), "corrupt");
}

TEST(Pipeline, SampleSessionThroughThePipelineMatchesTheGoldenEvents) {
  std::ifstream f(std::string(LOB_GOLDEN_DIR) + "/itch/session.events", std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  const Outcome o = run(lob::testkit::itch_samples::session_stream(), true, 2, 7,
                        lob::testkit::itch_samples::config());
  EXPECT_EQ(o.text, ss.str());
}

TEST(Pipeline, PacedReplayGivesTheSameResult) {
  tk::GeneratorParams gp;
  gp.seed = 5;
  gp.messages = 2000;
  const tk::Buf stream = tk::generate_stream(gp);
  expect_same(run(stream, false, 64, SIZE_MAX, target_config()),
              run(stream, true, 64, SIZE_MAX, target_config(), /*pace=*/2000), "paced");
}

TEST(Pipeline, FdSourceStreamsAFile) {
  tk::GeneratorParams gp;
  gp.seed = 3;
  gp.messages = 3000;
  const tk::Buf stream = tk::generate_stream(gp);
  char path[] = "/tmp/lob_itch_fd_XXXXXX";
  const int fd = mkstemp(path);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(write(fd, stream.data(), stream.size()), static_cast<ssize_t>(stream.size()));
  ASSERT_EQ(lseek(fd, 0, SEEK_SET), 0);
  pl::EventDigest digest;
  pl::BookStage<pl::EventDigest> stage{target_config(), digest};
  pl::FdSource src{fd};
  const pl::RunStats st = pl::run_pipelined(src, stage, pl::RunConfig{});
  close(fd);
  std::remove(path);
  const Outcome ref = run(stream, false, 64, SIZE_MAX, target_config());
  EXPECT_FALSE(st.source_error);
  EXPECT_EQ(digest.hash, ref.event_hash);
  EXPECT_EQ(stage.feed().book().state_hash(), ref.book_hash);
}

TEST(Pipeline, ChunkMustHoldTheLargestFrame) {
  pl::EventDigest digest;
  pl::BookStage<pl::EventDigest> stage{target_config(), digest};
  pl::MemorySource src{std::span<const std::uint8_t>{}};
  pl::RunConfig rc;
  rc.chunk_bytes = 1000;
  EXPECT_THROW(pl::run_single(src, stage, rc), std::invalid_argument);
}

// ---------------------------------------------------------------- latency recorder

TEST(LatencyRecorder, PercentilesSubtractOverheadAndClampAtZero) {
  pl::LatencyRecorder rec{1000};
  for (std::uint64_t i = 1; i <= 1000; ++i) rec.record(pl::kBook, i);
  pl::ClockCalibration c;
  c.ns_per_tick = 2.0;
  c.overhead_ticks = 10.0;
  const auto s = rec.summarize(pl::kBook, c);
  EXPECT_EQ(s.n, 1000U);
  EXPECT_DOUBLE_EQ(s.p50, (501.0 - 10.0) * 2.0);
  EXPECT_DOUBLE_EQ(s.p99, (991.0 - 10.0) * 2.0);
  EXPECT_DOUBLE_EQ(s.p999, (1000.0 - 10.0) * 2.0);
  EXPECT_DOUBLE_EQ(s.max, (1000.0 - 10.0) * 2.0);
  pl::LatencyRecorder small{2};
  for (int i = 0; i < 5; ++i) small.record(pl::kQueue, 3);
  const auto t = small.summarize(pl::kQueue, c);
  EXPECT_EQ(t.n, 2U);
  EXPECT_EQ(t.dropped, 3U);
  EXPECT_DOUBLE_EQ(t.p50, 0.0) << "3 ticks < 10 ticks overhead: clamped";
}

TEST(LatencyRecorder, PipelineRecordsEveryHop) {
  tk::GeneratorParams gp;
  gp.messages = 3000;
  const tk::Buf stream = tk::generate_stream(gp);
  pl::LatencyRecorder rec{10000};
  pl::EventDigest digest;
  pl::BookStage<pl::EventDigest> stage{target_config(), digest, &rec};
  pl::MemorySource src{stream};
  pl::run_pipelined(src, stage, pl::RunConfig{});
  const auto c = pl::calibrate(5);
  EXPECT_EQ(rec.summarize(pl::kDecode, c).n, 3000U);
  EXPECT_EQ(rec.summarize(pl::kQueue, c).n, 3000U);
  EXPECT_EQ(rec.summarize(pl::kBook, c).n, 3000U);
  EXPECT_GT(c.ns_per_tick, 0.0);
}

// ---------------------------------------------------------------- async logger

TEST(AsyncLogger, DeliversInOrderPerChannelAndFormats) {
  std::mutex m;
  std::string out;
  pl::AsyncLogger log{{"gap {}-{}", "book {} bid={}"}, 2, 1024, [&](std::string_view s) {
                        const std::lock_guard<std::mutex> g(m);
                        out += s;
                      }};
  log.start();
  std::thread a([&] {
    for (std::uint64_t i = 0; i < 500; ++i) {
      while (!log.channel(0).log(i, 1, {i, 100 + i})) std::this_thread::yield();
    }
  });
  std::thread b([&] {
    for (std::uint64_t i = 0; i < 500; ++i) {
      while (!log.channel(1).log(i, 0, {i, i + 1})) std::this_thread::yield();
    }
  });
  a.join();
  b.join();
  log.stop();
  EXPECT_EQ(log.written(), 1000U);
  // Per-channel order: "book i" lines appear with increasing i.
  std::istringstream in(out);
  std::string line;
  std::uint64_t next_book = 0;
  std::uint64_t next_gap = 0;
  while (std::getline(in, line)) {
    if (line.find(" book ") != std::string::npos) {
      EXPECT_EQ(line, std::to_string(next_book) + " book " + std::to_string(next_book) + " bid=" +
                          std::to_string(100 + next_book));
      ++next_book;
    } else {
      EXPECT_EQ(line, std::to_string(next_gap) + " gap " + std::to_string(next_gap) + "-" +
                          std::to_string(next_gap + 1));
      ++next_gap;
    }
  }
  EXPECT_EQ(next_book, 500U);
  EXPECT_EQ(next_gap, 500U);
}

TEST(AsyncLogger, FullChannelDropsInsteadOfBlocking) {
  std::string out;
  pl::AsyncLogger log{{"x {}"}, 1, 4, [&](std::string_view s) { out += s; }};
  // Writer not started: the queue fills and further records are dropped.
  int accepted = 0;
  for (std::uint64_t i = 0; i < 10; ++i) accepted += log.channel(0).log(i, 0, {i}) ? 1 : 0;
  EXPECT_EQ(accepted, 4);
  EXPECT_EQ(log.channel(0).dropped(), 6U);
  log.start();
  log.stop();
  EXPECT_EQ(out, "0 x 0\n1 x 1\n2 x 2\n3 x 3\n");
}

TEST(AsyncLogger, UnknownFormatAndMissingArgumentsAreVisible) {
  pl::AsyncLogger log{{"a {} b {}"}, 1, 4, [](std::string_view) {}};
  pl::LogRecord r;
  r.ts = 7;
  r.fmt = 0;
  r.nargs = 1;
  r.args[0] = 5;
  EXPECT_EQ(log.format(r), "7 a 5 b ?\n");
  r.fmt = 3;
  EXPECT_EQ(log.format(r), "7 <bad format 5>\n");
}

}  // namespace
