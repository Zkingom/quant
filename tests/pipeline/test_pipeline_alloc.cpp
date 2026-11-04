// L3' stage 2: the hot path allocates nothing. lob_testkit_alloc_guard
// replaces operator new in this executable; each worker thread arms the
// guard at the start of its hot loop (ThreadHooks) and disarms at the end.
// Any allocation in between is a violation and fails the test.
//
// Hot path = framing + decoding + SPSC push/pop + feed handler (Live, no
// gap) + book + downstream digest + async-log enqueue. Setup (buffers,
// queue, threads) happens before arming. Gap recovery allocates
// (retransmission buffer) and is deliberately outside the hot path.

#include <gtest/gtest.h>

#include <vector>

#include "lob/pipeline/async_logger.hpp"
#include "lob/pipeline/pipeline.hpp"
#include "lob/testkit/alloc_guard.hpp"
#include "lob/testkit/itch_generator.hpp"

namespace {

namespace pl = lob::pipeline;
namespace md = lob::md;
namespace tk = lob::testkit::itch;
namespace guard = lob::testkit::alloc_guard;

void arm() { guard::arm_this_thread(); }
void disarm() { guard::disarm_this_thread(); }

md::FeedConfig cfg() {
  md::FeedConfig c;
  c.symbol = md::make_symbol(tk::symbol_of(1));
  c.book = md::BookConfig{4096, 1024};
  return c;
}

tk::Buf stream() {
  tk::GeneratorParams gp;
  gp.seed = 17;
  gp.messages = 30000;
  return tk::generate_stream(gp);
}

TEST(HotPathAllocation, SingleThreadReplayAllocatesNothing) {
  const tk::Buf s = stream();
  pl::EventDigest digest;
  pl::LatencyRecorder rec{40000};
  pl::BookStage<pl::EventDigest> stage{cfg(), digest, &rec};
  pl::MemorySource src{s, 5000};
  pl::RunConfig rc;
  rc.hooks = pl::ThreadHooks{&arm, &disarm};
  guard::reset();
  const auto st = pl::run_single(src, stage, rc);
  EXPECT_EQ(st.frames, 30000U);
  EXPECT_GT(digest.book_updates, 1000U);
  EXPECT_EQ(guard::violations(), 0) << guard::violation_bytes() << " bytes";
  EXPECT_EQ(stage.feed().state(), md::FeedState::Live);
}

TEST(HotPathAllocation, PipelinedReplayAllocatesNothingInEitherThread) {
  const tk::Buf s = stream();
  pl::EventDigest digest;
  pl::LatencyRecorder rec{40000};
  pl::BookStage<pl::EventDigest> stage{cfg(), digest, &rec};
  pl::MemorySource src{s, 5000};
  pl::RunConfig rc;
  rc.queue_capacity = 256;
  rc.hooks = pl::ThreadHooks{&arm, &disarm};
  guard::reset();
  const auto st = pl::run_pipelined(src, stage, rc);
  EXPECT_EQ(st.frames, 30000U);
  EXPECT_EQ(guard::violations(), 0) << guard::violation_bytes() << " bytes";
  EXPECT_GT(guard::total_allocations(), 0) << "setup (queue, thread) allocates outside the hot loops";
}

TEST(HotPathAllocation, AsyncLogEnqueueAllocatesNothing) {
  pl::AsyncLogger log{{"m {} {}"}, 1, 1024, [](std::string_view) {}};
  log.start();
  guard::reset();
  arm();
  for (std::uint64_t i = 0; i < 5000; ++i) {
    (void)log.channel(0).log(i, 0, {i, i});
  }
  disarm();
  EXPECT_EQ(guard::violations(), 0);
  log.stop();
}

// Negative control: a downstream that allocates on the hot path must be caught.
struct AllocatingDownstream {
  std::vector<std::uint64_t> seen;  // no reserve: push_back reallocates
  void on_feed(const md::FeedInput& in, std::span<const md::FeedEvent>, const md::FeedHandler&,
               std::uint64_t, pl::LatencyRecorder*, bool) {
    seen.push_back(in.seq);
  }
};

TEST(HotPathAllocation, GuardDetectsAnAllocatingDownstream) {
  const tk::Buf s = stream();
  AllocatingDownstream ds;
  pl::BookStage<AllocatingDownstream> stage{cfg(), ds};
  pl::MemorySource src{s};
  pl::RunConfig rc;
  rc.hooks = pl::ThreadHooks{&arm, &disarm};
  guard::reset();
  pl::run_pipelined(src, stage, rc);
  EXPECT_GT(guard::violations(), 10);
}

}  // namespace
