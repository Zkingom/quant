// L3' 1d: feed handler -- instrument filter, gap detection, recovery state
// machine; MoldUDP64 framing; chunked decoding.

#include "lob/md/feed.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "lob/md/mold.hpp"
#include "lob/md/stream.hpp"
#include "lob/testkit/itch_builder.hpp"
#include "lob/testkit/itch_samples.hpp"

namespace {

namespace md = lob::md;
namespace tk = lob::testkit::itch;
using md::AlertReason;
using md::FeedEvent;
using md::FeedInput;
using md::FeedState;

constexpr std::uint16_t kLoc = lob::testkit::itch_samples::kLocate;
constexpr std::uint16_t kOther = lob::testkit::itch_samples::kOther;
using lob::testkit::itch_samples::config;
using lob::testkit::itch_samples::session;

std::vector<FeedInput> decode_all(const std::vector<tk::Buf>& msgs) {
  tk::Buf stream;
  for (const auto& m : msgs) tk::append_frame(stream, m);
  std::vector<FeedInput> out;
  const auto p = md::decode_frames(stream, 1, [&](const FeedInput& in) { out.push_back(in); });
  EXPECT_EQ(p.last, lob::md::itch::FrameStatus::End);
  return out;
}

std::string run_text(md::FeedHandler& h, const std::vector<FeedInput>& inputs) {
  std::vector<FeedEvent> ev;
  std::string text;
  for (const auto& in : inputs) {
    ev.clear();
    h.on_input(in, ev);
    for (const auto& e : ev) md::append_text(e, text);
  }
  return text;
}

// The complete expected event stream of session(), written out by hand.
TEST(Feed, GoldenEventStreamOfTheSampleSession) {
  md::FeedHandler h{config()};
  const std::string text = run_text(h, decode_all(session()));
  const std::string expected =
      "seq=1 ts=1 SYSTEM state=UNINITIALIZED reason=- detail=79 type=S ref=0 bid=0x0 ask=0x0\n"
      "seq=3 ts=3 STATE state=LIVE reason=- detail=0 type=R ref=0 bid=0x0 ask=0x0\n"
      "seq=4 ts=10 BOOK state=LIVE reason=- detail=0 type=A ref=1 bid=10000x100 ask=0x0 msg=B10000x100\n"
      "seq=6 ts=12 BOOK state=LIVE reason=- detail=0 type=A ref=3 bid=10000x100 ask=10100x200 msg=S10100x200\n"
      "seq=7 ts=13 BOOK state=LIVE reason=- detail=0 type=F ref=4 bid=10000x150 ask=10100x200 msg=B10000x50\n"
      "seq=9 ts=15 BOOK state=LIVE reason=- detail=0 type=E ref=1 bid=10000x110 ask=10100x200 msg=B10000x40\n"
      "seq=10 ts=16 BOOK state=LIVE reason=- detail=0 type=C ref=3 bid=10000x110 ask=10100x180 msg=S10100x20\n"
      "seq=11 ts=17 BOOK state=LIVE reason=- detail=0 type=X ref=4 bid=10000x100 ask=10100x180 msg=B10000x10\n"
      "seq=12 ts=18 BOOK state=LIVE reason=- detail=0 type=U ref=5 bid=10000x100 ask=10050x150 msg=S10050x150 prev=3@10100x180\n"
      "seq=14 ts=20 BOOK state=LIVE reason=- detail=0 type=D ref=1 bid=10000x40 ask=10050x150 msg=B10000x60\n"
      "seq=15 ts=21 SYSTEM state=LIVE reason=- detail=67 type=S ref=0 bid=10000x40 ask=10050x150\n";
  EXPECT_EQ(text, expected);
  EXPECT_EQ(h.state(), FeedState::Live);
  EXPECT_EQ(h.counters().applied, 8U);
  EXPECT_EQ(h.counters().other_stock, 2U);
  EXPECT_EQ(h.counters().skipped, 1U);
  EXPECT_EQ(h.book().find_invariant_violation(), std::nullopt);
}

TEST(Feed, OrdersBeforeTheDirectoryAreNotApplied) {
  md::FeedHandler h{config()};
  const auto in = decode_all({tk::add_order(kLoc, 1, 'B', 100, "ACME", 10000, 1),
                              tk::stock_directory(kLoc, "ACME", 2)});
  run_text(h, in);
  EXPECT_EQ(h.book().order_count(), 0U);
  EXPECT_EQ(h.counters().other_stock, 1U);
  EXPECT_EQ(h.state(), FeedState::Live);
}

TEST(Feed, SequenceGapWithoutRecoveryGoesStaleAndStopsPublishing) {
  md::FeedHandler h{config()};
  auto in = decode_all(session());
  in.erase(in.begin() + 5);  // drop seq 6 (ACME add of ref 3)
  std::vector<FeedEvent> ev;
  std::size_t books_after_gap = 0;
  bool gap_seen = false;
  for (const auto& x : in) {
    ev.clear();
    h.on_input(x, ev);
    for (const auto& e : ev) {
      if (e.reason == AlertReason::SequenceGap) {
        gap_seen = true;
        EXPECT_EQ(e.gap_first, 6U);
        EXPECT_EQ(e.gap_last, 6U);
        EXPECT_EQ(e.state, FeedState::Recovering);
      }
      if (gap_seen && e.kind == FeedEvent::Kind::BookUpdate) ++books_after_gap;
    }
  }
  EXPECT_TRUE(gap_seen);
  EXPECT_EQ(h.state(), FeedState::Stale);
  EXPECT_EQ(books_after_gap, 0U);
  EXPECT_EQ(h.counters().gaps, 1U);
}

class VectorRecovery final : public md::RecoverySource {
 public:
  explicit VectorRecovery(std::vector<FeedInput> all) : all_(std::move(all)) {}
  bool retransmit(std::uint64_t first, std::uint64_t last, std::vector<FeedInput>& out) override {
    ++calls;
    if (fail) return false;
    for (const auto& x : all_) {
      if (x.seq >= first && x.seq <= last) out.push_back(x);
    }
    if (drop_one && !out.empty()) out.pop_back();
    return true;
  }
  int calls = 0;
  bool fail = false;
  bool drop_one = false;

 private:
  std::vector<FeedInput> all_;
};

TEST(Feed, GapFilledByRetransmissionRecoversToTheSameBook) {
  const auto all = decode_all(session());
  md::FeedHandler reference{config()};
  run_text(reference, all);

  VectorRecovery rec{all};
  md::FeedHandler h{config(), &rec};
  auto in = all;
  in.erase(in.begin() + 4, in.begin() + 8);  // drop seq 5..8
  std::vector<FeedEvent> ev;
  std::vector<AlertReason> reasons;
  for (const auto& x : in) {
    ev.clear();
    h.on_input(x, ev);
    for (const auto& e : ev) {
      if (e.reason != AlertReason::None) reasons.push_back(e.reason);
      if (e.kind == FeedEvent::Kind::BookUpdate) {
        EXPECT_EQ(e.state, FeedState::Live);
      }
    }
  }
  EXPECT_EQ(rec.calls, 1);
  ASSERT_EQ(reasons, (std::vector<AlertReason>{AlertReason::SequenceGap, AlertReason::GapRecovered}));
  EXPECT_EQ(h.state(), FeedState::Live);
  EXPECT_EQ(h.book().state_hash(), reference.book().state_hash());
}

TEST(Feed, IncompleteRetransmissionIsAFailure) {
  const auto all = decode_all(session());
  VectorRecovery rec{all};
  rec.drop_one = true;
  md::FeedHandler h{config(), &rec};
  auto in = all;
  in.erase(in.begin() + 4, in.begin() + 6);
  run_text(h, in);
  EXPECT_EQ(h.state(), FeedState::Stale);
}

TEST(Feed, DuplicateSequenceNumbersAreDropped) {
  md::FeedHandler h{config()};
  auto in = decode_all(session());
  in.insert(in.begin() + 6, in[5]);  // replay seq 6 again
  run_text(h, in);
  EXPECT_EQ(h.state(), FeedState::Live);
  EXPECT_EQ(h.counters().duplicates, 1U);
  md::FeedHandler clean{config()};
  run_text(clean, decode_all(session()));
  EXPECT_EQ(h.book().state_hash(), clean.book().state_hash());
}

TEST(Feed, TimestampRegressionGoesStale) {
  md::FeedHandler h{config()};
  auto msgs = session();
  msgs[6] = tk::add_order_mpid(kLoc, 4, 'B', 50, "ACME", 10000, "MMID", 11);  // 11 < 12
  const std::string text = run_text(h, decode_all(msgs));
  EXPECT_EQ(h.state(), FeedState::Stale);
  EXPECT_NE(text.find("reason=TIMESTAMP_REGRESSION"), std::string::npos);
  EXPECT_EQ(text.find("BOOK state=STALE"), std::string::npos);
}

TEST(Feed, TimestampRegressionOnAnotherInstrumentAlsoCounts) {
  md::FeedHandler h{config()};
  auto msgs = session();
  msgs[12] = tk::order_delete(kOther, 2, 5);
  run_text(h, decode_all(msgs));
  EXPECT_EQ(h.state(), FeedState::Stale);
}

TEST(Feed, BookRejectionGoesStaleAndTheBookIsNotPublishedAgain) {
  for (const auto& bad : {tk::order_delete(kLoc, 99, 14),                       // unknown ref
                          tk::add_order(kLoc, 98, 'S', 10, "ACME", 9000, 14),   // crosses
                          tk::order_executed(kLoc, 1, 1000, 9, 14),             // overfill
                          tk::add_order(kLoc, 1, 'B', 10, "ACME", 9000, 14)}) { // duplicate
    md::FeedHandler h{config()};
    auto msgs = session();
    msgs[7] = bad;  // replaces the skipped trade message (seq 8)
    const std::string text = run_text(h, decode_all(msgs));
    EXPECT_EQ(h.state(), FeedState::Stale);
    EXPECT_NE(text.find("seq=8 ts=14 STATE state=STALE reason=BOOK_REJECTED"), std::string::npos) << text;
    EXPECT_EQ(text.find("seq=9 "), std::string::npos) << "nothing published after the rejection";
    EXPECT_EQ(h.counters().ignored_not_live, 7U);
  }
}

TEST(Feed, SymbolMismatchOnOurLocateGoesStale) {
  md::FeedHandler h{config()};
  auto msgs = session();
  msgs[7] = tk::add_order(kLoc, 50, 'B', 10, "ACMX", 9000, 14);
  run_text(h, decode_all(msgs));
  EXPECT_EQ(h.state(), FeedState::Stale);
}

TEST(Feed, DecodeAndFrameErrorsGoStale) {
  {
    md::FeedHandler h{config()};
    auto msgs = session();
    msgs[7] = tk::Buf{'A', 0, 1};  // truncated add order: BAD_LENGTH
    const std::string text = run_text(h, decode_all(msgs));
    EXPECT_EQ(h.state(), FeedState::Stale);
    EXPECT_NE(text.find("reason=DECODE_ERROR detail=4"), std::string::npos) << text;
  }
  {
    md::FeedHandler h{config()};
    std::vector<FeedEvent> ev;
    run_text(h, decode_all({tk::stock_directory(kLoc, "ACME", 1)}));
    FeedInput fe{};
    fe.seq = 2;
    fe.kind = md::InputKind::FrameError;
    h.on_input(fe, ev);
    ASSERT_EQ(ev.size(), 1U);
    EXPECT_EQ(ev[0].reason, AlertReason::FrameError);
    EXPECT_EQ(h.state(), FeedState::Stale);
  }
}

TEST(Feed, SnapshotResyncLeavesStale) {
  md::FeedHandler h{config()};
  auto msgs = session();
  msgs[7] = tk::order_delete(kLoc, 99, 14);
  run_text(h, decode_all(msgs));
  ASSERT_EQ(h.state(), FeedState::Stale);

  const std::vector<md::OrderView> snap{{21, md::Side::Buy, 10000, 300}, {22, md::Side::Sell, 10200, 100}};
  std::vector<FeedEvent> ev;
  h.resync_from_snapshot(snap, 100, ev);
  ASSERT_EQ(ev.size(), 1U);
  EXPECT_EQ(ev[0].reason, AlertReason::SnapshotLoaded);
  EXPECT_EQ(ev[0].bid_price, 10000U);
  EXPECT_EQ(h.state(), FeedState::Live);
  EXPECT_EQ(h.expected_seq(), 101U);

  ev.clear();
  const auto next = decode_all({tk::order_executed(kLoc, 21, 100, 5, 30)});
  FeedInput in = next[0];
  in.seq = 101;
  h.on_input(in, ev);
  ASSERT_EQ(ev.size(), 1U);
  EXPECT_EQ(ev[0].kind, FeedEvent::Kind::BookUpdate);
  EXPECT_EQ(ev[0].bid_shares, 200U);
}

TEST(Feed, CrossedSnapshotIsRejected) {
  md::FeedHandler h{config()};
  run_text(h, decode_all({tk::stock_directory(kLoc, "ACME", 1)}));
  const std::vector<md::OrderView> snap{{1, md::Side::Buy, 10000, 1}, {2, md::Side::Sell, 9000, 1}};
  std::vector<FeedEvent> ev;
  h.resync_from_snapshot(snap, 10, ev);
  EXPECT_EQ(h.state(), FeedState::Stale);
  EXPECT_EQ(ev.back().reason, AlertReason::SnapshotRejected);
  EXPECT_EQ(h.book().order_count(), 0U);
}

// ---------------------------------------------------------------- chunking

TEST(Stream, ChunkedDecodingAtEverySplitPointEqualsWholeBuffer) {
  tk::Buf stream;
  for (const auto& m : session()) tk::append_frame(stream, m);
  std::vector<std::uint64_t> whole;
  md::decode_frames(stream, 1, [&](const FeedInput& in) { whole.push_back(in.seq * 1000 + static_cast<std::uint64_t>(in.kind)); });
  for (std::size_t split = 0; split <= stream.size(); ++split) {
    std::vector<std::uint64_t> got;
    auto sink = [&](const FeedInput& in) { got.push_back(in.seq * 1000 + static_cast<std::uint64_t>(in.kind)); };
    const lob::md::itch::Bytes all{stream};
    const auto p1 = md::decode_frames(all.subspan(0, split), 1, sink);
    // Carry the unconsumed tail into the second chunk.
    const auto p2 = md::decode_frames(all.subspan(p1.consumed), p1.next_seq, sink);
    EXPECT_EQ(p2.last, lob::md::itch::FrameStatus::End) << split;
    EXPECT_EQ(got, whole) << split;
  }
}

// ---------------------------------------------------------------- MoldUDP64

tk::Buf mold_packet(std::uint64_t seq, const std::vector<tk::Buf>& msgs, bool end = false) {
  tk::Buf p;
  for (const char c : std::string("SESSION001")) p.push_back(static_cast<std::uint8_t>(c));
  for (int i = 7; i >= 0; --i) p.push_back(static_cast<std::uint8_t>((seq >> (8 * i)) & 0xff));
  const std::uint16_t count = end ? 0xFFFF : static_cast<std::uint16_t>(msgs.size());
  p.push_back(static_cast<std::uint8_t>(count >> 8));
  p.push_back(static_cast<std::uint8_t>(count & 0xff));
  for (const auto& m : msgs) tk::append_frame(p, m);
  return p;
}

TEST(Mold, ParsesHeaderAndAssignsConsecutiveSequenceNumbers) {
  const auto pkt = mold_packet(41, {tk::order_delete(1, 1), tk::order_delete(1, 2), tk::order_delete(1, 3)});
  md::mold::Header h;
  std::vector<std::uint64_t> seqs;
  EXPECT_EQ(md::mold::parse_packet(pkt, h, [&](std::uint64_t s, lob::md::itch::Bytes p) {
              seqs.push_back(s);
              EXPECT_EQ(p.size(), 19U);
            }),
            md::mold::Status::Ok);
  EXPECT_EQ(std::string(h.session.data(), 10), "SESSION001");
  EXPECT_EQ(h.sequence, 41U);
  EXPECT_EQ(seqs, (std::vector<std::uint64_t>{41, 42, 43}));
}

TEST(Mold, HeartbeatEndOfSessionAndDamage) {
  md::mold::Header h;
  auto none = [](std::uint64_t, lob::md::itch::Bytes) {};
  EXPECT_EQ(md::mold::parse_packet(mold_packet(5, {}), h, none), md::mold::Status::Heartbeat);
  EXPECT_EQ(md::mold::parse_packet(mold_packet(5, {}, true), h, none), md::mold::Status::EndOfSession);
  auto pkt = mold_packet(5, {tk::order_delete(1, 1)});
  for (std::size_t cut = 0; cut < pkt.size(); ++cut) {
    const lob::md::itch::Bytes b{pkt.data(), cut};
    EXPECT_EQ(md::mold::parse_packet(b, h, none), md::mold::Status::Truncated) << cut;
  }
  pkt.push_back(0);
  EXPECT_EQ(md::mold::parse_packet(pkt, h, none), md::mold::Status::TrailingBytes);
  auto zero = mold_packet(5, {tk::order_delete(1, 1)});
  zero[20] = 0;
  zero[21] = 0;
  EXPECT_EQ(md::mold::parse_packet(zero, h, none), md::mold::Status::ZeroLength);
}

TEST(Mold, LostPacketIsDetectedAsAnExactSequenceGap) {
  const auto msgs = session();
  // Packets of 3 messages; packet 2 (seq 4..6) is lost.
  std::vector<tk::Buf> packets;
  for (std::size_t i = 0; i < msgs.size(); i += 3) {
    std::vector<tk::Buf> chunk(msgs.begin() + static_cast<long>(i),
                               msgs.begin() + static_cast<long>(std::min(i + 3, msgs.size())));
    packets.push_back(mold_packet(i + 1, chunk));
  }
  packets.erase(packets.begin() + 1);
  md::FeedHandler h{config()};
  std::vector<FeedEvent> ev;
  for (const auto& pkt : packets) {
    md::mold::Header hdr;
    ASSERT_EQ(md::mold::parse_packet(pkt, hdr, [&](std::uint64_t seq, lob::md::itch::Bytes p) {
                FeedInput in{};
                in.seq = seq;
                const auto ds = lob::md::itch::decode(p, in.msg);
                in.kind = ds == lob::md::itch::DecodeStatus::Decoded   ? md::InputKind::Decoded
                          : ds == lob::md::itch::DecodeStatus::Skipped ? md::InputKind::Skipped
                                                                       : md::InputKind::DecodeError;
                h.on_input(in, ev);
              }),
              md::mold::Status::Ok);
  }
  bool found = false;
  for (const auto& e : ev) {
    if (e.reason == AlertReason::SequenceGap) {
      found = true;
      EXPECT_EQ(e.gap_first, 4U);
      EXPECT_EQ(e.gap_last, 6U);
    }
  }
  EXPECT_TRUE(found);
  EXPECT_EQ(h.state(), FeedState::Stale);
}

}  // namespace
