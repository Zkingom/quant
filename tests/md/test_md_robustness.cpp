// L3' 1e: robustness of the market-data path against malformed input.
// Only hand-built samples are used (no real ITCH data in the repository).
//
//   - golden: the sample session's bytes and event stream are pinned to
//     reviewable files under tests/golden/itch/;
//   - byte-by-byte truncation of the stream and of every message payload;
//   - deterministic mutational fuzzing (fixed seeds, runs in every preset;
//     the ASan/UBSan presets turn any out-of-bounds read or UB into a failure).
// Pass criterion everywhere: invalid input is rejected (an error status, or
// the feed goes Stale) -- never a crash, never an inconsistent book, and
// never a BookUpdate from a feed that is not Live.

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "lob/md/feed.hpp"
#include "lob/md/stream.hpp"
#include "lob/testkit/itch_builder.hpp"
#include "lob/testkit/itch_samples.hpp"
#include "lob/testkit/rng.hpp"

namespace {

namespace md = lob::md;
namespace itch = lob::md::itch;
namespace tk = lob::testkit::itch;
using md::FeedEvent;
using md::FeedInput;
using md::FeedState;

std::string read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::string to_hex_lines(const tk::Buf& stream) {
  // One frame per line: "LLLL payload..." in lowercase hex.
  std::string out;
  itch::FrameCursor cur{stream};
  itch::Bytes p;
  static const char* digits = "0123456789abcdef";
  std::size_t prev = 0;
  while (cur.next(p) == itch::FrameStatus::Frame) {
    for (std::size_t i = prev; i < cur.offset(); ++i) {
      if (i == prev + 2) out += ' ';
      out += digits[stream[i] >> 4U];
      out += digits[stream[i] & 0xfU];
    }
    out += '\n';
    prev = cur.offset();
  }
  return out;
}

struct RunResult {
  std::string text;
  FeedState state{};
  bool book_ok{true};
  bool published_while_not_live{false};
  itch::FrameStatus last{};
};

/// Decode + feed exactly like file replay: a truncated tail or a
/// zero-length prefix at end of input becomes a FrameError input.
RunResult run_stream(itch::Bytes stream) {
  md::FeedHandler h{lob::testkit::itch_samples::config()};
  RunResult r;
  std::vector<FeedEvent> ev;
  auto sink = [&](const FeedInput& in) {
    ev.clear();
    h.on_input(in, ev);
    for (const auto& e : ev) {
      md::append_text(e, r.text);
      if (e.kind == FeedEvent::Kind::BookUpdate && e.state != FeedState::Live) {
        r.published_while_not_live = true;
      }
    }
  };
  const auto p = md::decode_frames(stream, 1, sink);
  r.last = p.last;
  if (p.last != itch::FrameStatus::End) {
    FeedInput fe{};
    fe.seq = p.next_seq;
    fe.kind = md::InputKind::FrameError;
    sink(fe);
  }
  r.state = h.state();
  r.book_ok = !h.book().find_invariant_violation().has_value();
  return r;
}

TEST(MdGolden, SampleSessionBytesAndEventsMatchCommittedFiles) {
  const tk::Buf stream = lob::testkit::itch_samples::session_stream();
  const std::string hex_path = std::string(LOB_GOLDEN_DIR) + "/itch/session.hex";
  const std::string ev_path = std::string(LOB_GOLDEN_DIR) + "/itch/session.events";
  const std::string hex = to_hex_lines(stream);
  const RunResult r = run_stream(stream);
  const std::string want_hex = read_file(hex_path);
  const std::string want_ev = read_file(ev_path);
  if (hex != want_hex || r.text != want_ev) {
    std::ofstream(hex_path + ".actual", std::ios::binary) << hex;
    std::ofstream(ev_path + ".actual", std::ios::binary) << r.text;
  }
  EXPECT_EQ(hex, want_hex) << "see " << hex_path << ".actual";
  EXPECT_EQ(r.text, want_ev) << "see " << ev_path << ".actual";
  EXPECT_EQ(r.state, FeedState::Live);
}

TEST(MdTruncation, EveryPrefixOfTheStreamIsHandled) {
  const tk::Buf stream = lob::testkit::itch_samples::session_stream();
  const RunResult full = run_stream(stream);
  ASSERT_EQ(full.state, FeedState::Live);
  std::size_t frame_boundaries = 0;
  for (std::size_t cut = 0; cut < stream.size(); ++cut) {
    const RunResult r = run_stream(itch::Bytes{stream.data(), cut});
    EXPECT_TRUE(r.book_ok) << cut;
    EXPECT_FALSE(r.published_while_not_live) << cut;
    if (r.last == itch::FrameStatus::End) {
      ++frame_boundaries;  // a clean prefix: its events are a prefix of the full run
      EXPECT_EQ(full.text.compare(0, r.text.size(), r.text), 0) << cut;
    } else {
      EXPECT_EQ(r.last, itch::FrameStatus::Truncated) << cut;
      EXPECT_EQ(r.state, FeedState::Stale) << "a truncated tail must not leave the feed usable: " << cut;
    }
  }
  EXPECT_EQ(frame_boundaries, lob::testkit::itch_samples::session().size());
}

TEST(MdTruncation, EveryShortenedPayloadIsRejectedByTheDecoder) {
  for (const tk::Buf& msg : lob::testkit::itch_samples::session()) {
    for (std::size_t len = 0; len < msg.size(); ++len) {
      itch::Message m = itch::OrderDelete{};
      const auto s = itch::decode(itch::Bytes{msg.data(), len}, m);
      EXPECT_TRUE(s == itch::DecodeStatus::Empty || s == itch::DecodeStatus::BadLength)
          << msg[0] << " len " << len;
      EXPECT_TRUE(std::holds_alternative<itch::OrderDelete>(m));
    }
  }
}

// Mutations: flip a bit, overwrite a byte, insert or delete a byte, rewrite a
// length prefix, duplicate or swap whole frames.
tk::Buf mutate(const tk::Buf& base, lob::testkit::Rng& rng) {
  tk::Buf b = base;
  const int n = static_cast<int>(rng.uniform(1, 4));
  for (int k = 0; k < n && !b.empty(); ++k) {
    const auto pos = static_cast<std::size_t>(rng.uniform(0, static_cast<std::int64_t>(b.size()) - 1));
    switch (rng.uniform(0, 5)) {
      case 0:
        b[pos] = static_cast<std::uint8_t>(b[pos] ^ (1U << rng.uniform(0, 7)));
        break;
      case 1:
        b[pos] = static_cast<std::uint8_t>(rng.uniform(0, 255));
        break;
      case 2:
        b.insert(b.begin() + static_cast<long>(pos), static_cast<std::uint8_t>(rng.uniform(0, 255)));
        break;
      case 3:
        b.erase(b.begin() + static_cast<long>(pos));
        break;
      case 4: {  // a plausible but wrong length prefix at a frame boundary
        itch::FrameCursor cur{b};
        itch::Bytes p;
        std::size_t target = 0;
        const auto skip = rng.uniform(0, 14);
        for (std::int64_t i = 0; i < skip && cur.next(p) == itch::FrameStatus::Frame; ++i) {
          target = cur.offset();
        }
        if (target + 1 < b.size()) {
          b[target + 1] = static_cast<std::uint8_t>(b[target + 1] + rng.uniform(-3, 3));
        }
        break;
      }
      default: {  // duplicate a slice (frames out of order / replayed)
        const auto len = static_cast<std::size_t>(rng.uniform(1, 60));
        const auto end = std::min(b.size(), pos + len);
        const tk::Buf slice(b.begin() + static_cast<long>(pos), b.begin() + static_cast<long>(end));
        const auto at = static_cast<std::size_t>(rng.uniform(0, static_cast<std::int64_t>(b.size())));
        b.insert(b.begin() + static_cast<long>(at), slice.begin(), slice.end());
        break;
      }
    }
  }
  return b;
}

#if defined(__SANITIZE_ADDRESS__)
constexpr int kFuzzIterations = 4000;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr int kFuzzIterations = 4000;
#else
constexpr int kFuzzIterations = 20000;
#endif
#else
constexpr int kFuzzIterations = 20000;
#endif

TEST(MdFuzz, MutatedStreamsNeverCrashOrCorruptTheBook) {
  const tk::Buf base = lob::testkit::itch_samples::session_stream();
  const std::string clean = run_stream(base).text;
  int changed = 0;
  int stale = 0;
  for (std::uint64_t seed = 1; seed <= 4; ++seed) {
    lob::testkit::Rng rng{seed};
    for (int i = 0; i < kFuzzIterations / 4; ++i) {
      const tk::Buf b = mutate(base, rng);
      const RunResult r = run_stream(b);
      ASSERT_TRUE(r.book_ok) << "seed " << seed << " iter " << i;
      ASSERT_FALSE(r.published_while_not_live) << "seed " << seed << " iter " << i;
      changed += r.text != clean ? 1 : 0;
      stale += r.state == FeedState::Stale ? 1 : 0;
    }
  }
  // The mutator must actually perturb the outcome, and most perturbations
  // must be caught as errors.
  EXPECT_GT(changed, kFuzzIterations / 2);
  EXPECT_GT(stale, kFuzzIterations / 3);
}

TEST(MdFuzz, RandomPayloadsOfEveryTypeAndLength) {
  lob::testkit::Rng rng{77};
  long decoded = 0;
  for (int t = 0; t < 256; ++t) {
    for (std::size_t len = 0; len <= 64; ++len) {
      for (int k = 0; k < 4; ++k) {
        tk::Buf b(len);
        for (auto& x : b) x = static_cast<std::uint8_t>(rng.uniform(0, 255));
        if (!b.empty()) b[0] = static_cast<std::uint8_t>(t);
        itch::Message m;
        const auto s = itch::decode(b, m);
        if (s == itch::DecodeStatus::Decoded) {
          ++decoded;
          EXPECT_EQ(len, itch::spec_length(static_cast<char>(t)));
          EXPECT_LT(itch::header_of(m).timestamp, itch::kNanosPerDay);
        }
      }
    }
  }
  EXPECT_GT(decoded, 0);
}

}  // namespace
