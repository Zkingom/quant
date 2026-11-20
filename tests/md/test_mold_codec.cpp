// L5.1: MoldUDP64 encoding -- downstream packet builder / packetizer,
// heartbeat and end-of-session, request packet codec.
//
//   - hand-written byte samples at the offsets of the Nasdaq MoldUDP64
//     specification v1.00 pin the encoder and the existing decoder at once;
//   - builder limits: maximum packet size (exact fit and one byte over),
//     message length that can never fit, count capped at 0xFFFE;
//   - packetizer properties on fixed-seed random inputs: concatenation of
//     the packets equals the input, every packet within the limit, sequence
//     numbers contiguous, packets filled greedily;
//   - byte-by-byte truncation and trailing bytes of every packet kind;
//   - fixed-seed fuzz: random bytes never crash the parsers (ASan/UBSan
//     presets), and whatever parses as Ok re-encodes to the same bytes.
// Only synthetic bytes are used (no real ITCH data in the repository).

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

#include "lob/md/mold.hpp"
#include "lob/testkit/rng.hpp"

namespace {

namespace mold = lob::md::mold;
using lob::md::itch::Bytes;
using Buf = std::vector<std::uint8_t>;

mold::Session session(std::string_view s) {
  mold::Session out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = s[i];
  }
  return out;
}

Buf bytes_of(Bytes b) { return Buf(b.begin(), b.end()); }

struct Parsed {
  mold::Status status{};
  mold::Header header{};
  std::vector<std::uint64_t> seqs;
  std::vector<Buf> msgs;
};

Parsed parse(Bytes pkt) {
  Parsed p;
  p.status = mold::parse_packet(pkt, p.header, [&](std::uint64_t s, Bytes m) {
    p.seqs.push_back(s);
    p.msgs.push_back(bytes_of(m));
  });
  return p;
}

// ---------------------------------------------------------------------------
// Hand-written samples (spec offsets: Session 0..9, Sequence 10..17,
// Count 18..19, then Length 2 + Data per message; request = same 20 bytes).

const Buf kSampleSession = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J'};

Buf sample(std::initializer_list<std::uint8_t> tail) {
  Buf b = kSampleSession;
  b.insert(b.end(), tail);
  return b;
}

TEST(MoldCodec, HandWrittenDownstreamPacketMatchesEncoderAndDecoder) {
  const Buf expected = sample({0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,  // sequence
                               0x00, 0x02,                                      // count
                               0x00, 0x01, 0xAA,                                // len 1
                               0x00, 0x03, 0xBB, 0xCC, 0xDD});                  // len 3
  Buf buf(mold::kDefaultMaxPacketSize);
  mold::PacketBuilder b{buf, session("ABCDEFGHIJ"), 0x0102030405060708ULL};
  const Buf m1 = {0xAA};
  const Buf m2 = {0xBB, 0xCC, 0xDD};
  ASSERT_EQ(b.add(m1), mold::PacketBuilder::Add::Ok);
  ASSERT_EQ(b.add(m2), mold::PacketBuilder::Add::Ok);
  EXPECT_EQ(b.count(), 2U);
  EXPECT_EQ(b.size(), expected.size());
  EXPECT_EQ(bytes_of(b.finish()), expected);

  const Parsed p = parse(expected);
  ASSERT_EQ(p.status, mold::Status::Ok);
  EXPECT_EQ(p.header.session, session("ABCDEFGHIJ"));
  EXPECT_EQ(p.header.sequence, 0x0102030405060708ULL);
  EXPECT_EQ(p.header.count, 2U);
  EXPECT_EQ(p.seqs, (std::vector<std::uint64_t>{0x0102030405060708ULL, 0x0102030405060709ULL}));
  EXPECT_EQ(p.msgs, (std::vector<Buf>{m1, m2}));
}

TEST(MoldCodec, HandWrittenHeartbeatAndEndOfSession) {
  // Both carry the sequence number of the next message to be sent.
  const Buf heartbeat = sample({0, 0, 0, 0, 0, 0, 0, 0x64, 0x00, 0x00});
  const Buf end = sample({0, 0, 0, 0, 0, 0, 0, 0x64, 0xFF, 0xFF});
  Buf buf(mold::kHeaderSize);
  ASSERT_EQ(mold::encode_heartbeat(buf, session("ABCDEFGHIJ"), 100), mold::kHeaderSize);
  EXPECT_EQ(buf, heartbeat);
  ASSERT_EQ(mold::encode_end_of_session(buf, session("ABCDEFGHIJ"), 100), mold::kHeaderSize);
  EXPECT_EQ(buf, end);

  Parsed p = parse(heartbeat);
  EXPECT_EQ(p.status, mold::Status::Heartbeat);
  EXPECT_EQ(p.header.sequence, 100U);
  p = parse(end);
  EXPECT_EQ(p.status, mold::Status::EndOfSession);
  EXPECT_EQ(p.header.sequence, 100U);
}

TEST(MoldCodec, HandWrittenRequestPacket) {
  const Buf expected = sample({0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03,  // first requested seq
                               0x01, 0x2C});                                    // 300 messages
  const mold::Request r{session("ABCDEFGHIJ"), 0x010203ULL, 300};
  Buf buf(mold::kRequestSize);
  ASSERT_EQ(mold::encode_request(buf, r), mold::kRequestSize);
  EXPECT_EQ(buf, expected);

  mold::Request back{};
  ASSERT_EQ(mold::parse_request(expected, back), mold::RequestStatus::Ok);
  EXPECT_EQ(back.session, r.session);
  EXPECT_EQ(back.sequence, r.sequence);
  EXPECT_EQ(back.count, r.count);
}

TEST(MoldCodec, EncodersWriteNothingIntoTooSmallBuffers) {
  Buf small(mold::kHeaderSize - 1, 0x5A);
  const Buf untouched = small;
  EXPECT_EQ(mold::encode_heartbeat(small, session("ABCDEFGHIJ"), 1), 0U);
  EXPECT_EQ(mold::encode_end_of_session(small, session("ABCDEFGHIJ"), 1), 0U);
  EXPECT_EQ(mold::encode_request(small, mold::Request{session("ABCDEFGHIJ"), 1, 1}), 0U);
  EXPECT_EQ(small, untouched);
}

// ---------------------------------------------------------------------------
// Builder limits.

TEST(MoldCodec, BuilderFillsThePacketExactlyToTheLimitAndNoFurther) {
  // 1 400 = 20 header + 4 blocks of (2 + 343).
  Buf buf(mold::kDefaultMaxPacketSize);
  mold::PacketBuilder b{buf, session("ABCDEFGHIJ"), 7};
  const Buf m(343, 0x11);
  for (int i = 0; i < 4; ++i) {
    ASSERT_EQ(b.add(m), mold::PacketBuilder::Add::Ok) << i;
  }
  EXPECT_EQ(b.size(), mold::kDefaultMaxPacketSize);
  const Buf one = {0x22};
  EXPECT_EQ(b.add(one), mold::PacketBuilder::Add::Full);  // 3 more bytes do not fit
  EXPECT_EQ(b.count(), 4U);                                // a refused add changes nothing
  EXPECT_EQ(b.size(), mold::kDefaultMaxPacketSize);
  EXPECT_EQ(b.next_seq(), 11U);

  const Parsed p = parse(b.finish());
  ASSERT_EQ(p.status, mold::Status::Ok);
  EXPECT_EQ(p.seqs, (std::vector<std::uint64_t>{7, 8, 9, 10}));

  // finish() starts the next packet at the next sequence number.
  EXPECT_TRUE(b.empty());
  EXPECT_EQ(b.first_seq(), 11U);
  ASSERT_EQ(b.add(one), mold::PacketBuilder::Add::Ok);
  const Parsed q = parse(b.finish());
  ASSERT_EQ(q.status, mold::Status::Ok);
  EXPECT_EQ(q.seqs, (std::vector<std::uint64_t>{11}));
}

TEST(MoldCodec, BuilderRejectsMessagesThatCanNeverFit) {
  Buf buf(mold::kDefaultMaxPacketSize);
  mold::PacketBuilder b{buf, session("ABCDEFGHIJ"), 1};
  const Buf too_long(mold::kDefaultMaxPacketSize - mold::kHeaderSize - 1, 0x33);  // 2 + 1379 > 1380
  const Buf longest(mold::kDefaultMaxPacketSize - mold::kHeaderSize - 2, 0x33);   // 2 + 1378 = 1380
  EXPECT_EQ(b.add(too_long), mold::PacketBuilder::Add::TooLarge);
  EXPECT_EQ(b.add(Buf{}), mold::PacketBuilder::Add::ZeroLength);
  EXPECT_TRUE(b.empty());
  ASSERT_EQ(b.add(longest), mold::PacketBuilder::Add::Ok);
  EXPECT_EQ(b.size(), mold::kDefaultMaxPacketSize);
  // TooLarge is reported even when the current packet is not empty.
  EXPECT_EQ(b.add(too_long), mold::PacketBuilder::Add::TooLarge);

  // The length field is 2 bytes: 65 536 bytes cannot be framed in any buffer.
  Buf big(mold::kHeaderSize + 2 + 70'000);
  mold::PacketBuilder bb{big, session("ABCDEFGHIJ"), 1};
  EXPECT_EQ(bb.add(Buf(65'536, 1)), mold::PacketBuilder::Add::TooLarge);
  EXPECT_EQ(bb.add(Buf(65'535, 1)), mold::PacketBuilder::Add::Ok);
}

TEST(MoldCodec, BuilderCapsTheCountBelowTheEndOfSessionMarker) {
  // A count of 0xFFFF would read as end of session; the builder stops at 0xFFFE.
  Buf buf(mold::kHeaderSize + 3 * 0x10000);
  mold::PacketBuilder b{buf, session("ABCDEFGHIJ"), 1};
  const Buf m = {0x44};
  for (std::uint32_t i = 0; i < mold::kMaxMessageCount; ++i) {
    ASSERT_EQ(b.add(m), mold::PacketBuilder::Add::Ok) << i;
  }
  EXPECT_EQ(b.add(m), mold::PacketBuilder::Add::Full);
  const Parsed p = parse(b.finish());
  ASSERT_EQ(p.status, mold::Status::Ok);
  EXPECT_EQ(p.header.count, mold::kMaxMessageCount);
  EXPECT_EQ(p.seqs.back(), mold::kMaxMessageCount);
}

// ---------------------------------------------------------------------------
// Packetizer properties.

struct Packetized {
  std::vector<Buf> packets;
  mold::PacketizeResult result{};
};

Packetized run_packetize(const std::vector<Buf>& msgs, std::size_t max_packet, std::uint64_t first_seq) {
  Packetized out;
  Buf buf(max_packet);
  out.result = mold::packetize(msgs, buf, session("SESSION001"), first_seq,
                               [&](Bytes pkt) { out.packets.push_back(bytes_of(pkt)); });
  return out;
}

void expect_packetization(const std::vector<Buf>& msgs, std::size_t count, const Packetized& got,
                          std::size_t max_packet, std::uint64_t first_seq) {
  std::uint64_t next = first_seq;
  std::vector<Buf> delivered;
  for (std::size_t i = 0; i < got.packets.size(); ++i) {
    const Buf& pkt = got.packets[i];
    ASSERT_LE(pkt.size(), max_packet);
    const Parsed p = parse(pkt);
    ASSERT_EQ(p.status, mold::Status::Ok) << "packet " << i;
    ASSERT_GE(p.header.count, 1U);
    EXPECT_EQ(p.header.session, session("SESSION001"));
    EXPECT_EQ(p.header.sequence, next);
    for (std::size_t k = 0; k < p.seqs.size(); ++k) {
      EXPECT_EQ(p.seqs[k], next + k);
    }
    next += p.header.count;
    delivered.insert(delivered.end(), p.msgs.begin(), p.msgs.end());
    // Greedy: every packet but the last was closed only because the next
    // message did not fit (or the count cap was reached).
    if (i + 1 < got.packets.size()) {
      const std::size_t following = msgs[delivered.size()].size();
      EXPECT_TRUE(pkt.size() + 2 + following > max_packet || p.header.count == mold::kMaxMessageCount)
          << "packet " << i << " closed early";
    }
  }
  EXPECT_EQ(delivered, std::vector<Buf>(msgs.begin(), msgs.begin() + static_cast<std::ptrdiff_t>(count)));
}

TEST(MoldCodec, PacketizerPropertiesOnRandomInputs) {
  lob::testkit::Rng rng{0x4D4F4C44};  // "MOLD"
  for (int trial = 0; trial < 400; ++trial) {
    const auto max_packet = static_cast<std::size_t>(
        rng.percent(30) ? static_cast<std::int64_t>(mold::kDefaultMaxPacketSize) : rng.uniform(23, 1400));
    const std::size_t longest = max_packet - mold::kHeaderSize - 2;
    const auto n = static_cast<std::size_t>(rng.uniform(0, 300));
    std::vector<Buf> msgs(n);
    for (auto& m : msgs) {
      const std::int64_t len = rng.percent(5) ? static_cast<std::int64_t>(longest)
                                              : rng.uniform(1, std::min<std::int64_t>(50, static_cast<std::int64_t>(longest)));
      m.resize(static_cast<std::size_t>(len));
      for (auto& byte : m) {
        byte = static_cast<std::uint8_t>(rng.uniform(0, 255));
      }
    }
    const auto first_seq = static_cast<std::uint64_t>(rng.uniform(1, 1'000'000'000));
    const Packetized got = run_packetize(msgs, max_packet, first_seq);
    ASSERT_TRUE(got.result.ok) << "trial " << trial;
    EXPECT_EQ(got.result.next_seq, first_seq + n);
    ASSERT_NO_FATAL_FAILURE(expect_packetization(msgs, n, got, max_packet, first_seq)) << "trial " << trial;
    EXPECT_EQ(got.packets.empty(), n == 0);
  }
}

TEST(MoldCodec, PacketizerStopsAtTheFirstUnframableMessageAfterFlushingThePrefix) {
  std::vector<Buf> msgs = {Buf(10, 1), Buf(20, 2), Buf(30, 3), Buf(2000, 4), Buf(5, 5)};
  const Packetized got = run_packetize(msgs, mold::kDefaultMaxPacketSize, 1);
  EXPECT_FALSE(got.result.ok);
  EXPECT_EQ(got.result.failed_index, 3U);
  EXPECT_EQ(got.result.reason, mold::PacketBuilder::Add::TooLarge);
  EXPECT_EQ(got.result.next_seq, 4U);
  ASSERT_NO_FATAL_FAILURE(expect_packetization(msgs, 3, got, mold::kDefaultMaxPacketSize, 1));

  msgs[1].clear();
  const Packetized zero = run_packetize(msgs, mold::kDefaultMaxPacketSize, 1);
  EXPECT_FALSE(zero.result.ok);
  EXPECT_EQ(zero.result.failed_index, 1U);
  EXPECT_EQ(zero.result.reason, mold::PacketBuilder::Add::ZeroLength);
  ASSERT_NO_FATAL_FAILURE(expect_packetization(msgs, 1, zero, mold::kDefaultMaxPacketSize, 1));
}

// ---------------------------------------------------------------------------
// Truncation and trailing bytes.

TEST(MoldCodec, EveryTruncationOfAnEncodedPacketIsReported) {
  Buf buf(mold::kDefaultMaxPacketSize);
  mold::PacketBuilder b{buf, session("ABCDEFGHIJ"), 5};
  for (std::uint8_t len = 1; len <= 6; ++len) {
    ASSERT_EQ(b.add(Buf(len, len)), mold::PacketBuilder::Add::Ok);
  }
  Buf pkt = bytes_of(b.finish());
  mold::Header h;
  auto none = [](std::uint64_t, Bytes) {};
  for (std::size_t cut = 0; cut < pkt.size(); ++cut) {
    EXPECT_EQ(mold::parse_packet(Bytes{pkt.data(), cut}, h, none), mold::Status::Truncated) << cut;
  }
  pkt.push_back(0);
  EXPECT_EQ(mold::parse_packet(pkt, h, none), mold::Status::TrailingBytes);

  Buf hb(mold::kHeaderSize + 1, 0);
  ASSERT_EQ(mold::encode_heartbeat(hb, session("ABCDEFGHIJ"), 1), mold::kHeaderSize);
  EXPECT_EQ(mold::parse_packet(hb, h, none), mold::Status::TrailingBytes);
  ASSERT_EQ(mold::encode_end_of_session(hb, session("ABCDEFGHIJ"), 1), mold::kHeaderSize);
  EXPECT_EQ(mold::parse_packet(hb, h, none), mold::Status::TrailingBytes);
  for (std::size_t cut = 0; cut < mold::kHeaderSize; ++cut) {
    EXPECT_EQ(mold::parse_packet(Bytes{hb.data(), cut}, h, none), mold::Status::Truncated) << cut;
  }
}

TEST(MoldCodec, RequestTruncationTrailingBytesAndZeroCount) {
  Buf buf(mold::kRequestSize + 1, 0);
  ASSERT_EQ(mold::encode_request(buf, mold::Request{session("ABCDEFGHIJ"), 9, 4}), mold::kRequestSize);
  mold::Request r{};
  for (std::size_t cut = 0; cut < mold::kRequestSize; ++cut) {
    EXPECT_EQ(mold::parse_request(Bytes{buf.data(), cut}, r), mold::RequestStatus::Truncated) << cut;
  }
  EXPECT_EQ(mold::parse_request(buf, r), mold::RequestStatus::TrailingBytes);
  EXPECT_EQ(mold::parse_request(Bytes{buf.data(), mold::kRequestSize}, r), mold::RequestStatus::Ok);

  ASSERT_EQ(mold::encode_request(buf, mold::Request{session("ABCDEFGHIJ"), 9, 0}), mold::kRequestSize);
  EXPECT_EQ(mold::parse_request(Bytes{buf.data(), mold::kRequestSize}, r), mold::RequestStatus::ZeroCount);
}

// ---------------------------------------------------------------------------
// Fixed-seed fuzz.

TEST(MoldCodec, RequestRoundTripOnRandomValues) {
  lob::testkit::Rng rng{0x52455155};  // "REQU"
  Buf buf(mold::kRequestSize);
  for (int i = 0; i < 2000; ++i) {
    mold::Request r{};
    for (auto& c : r.session) {
      c = static_cast<char>(rng.uniform(0x20, 0x7E));
    }
    r.sequence = static_cast<std::uint64_t>(rng.uniform(0, INT64_MAX)) * (rng.percent(50) ? 2U : 1U);
    r.count = static_cast<std::uint16_t>(rng.uniform(1, 0xFFFF));
    ASSERT_EQ(mold::encode_request(buf, r), mold::kRequestSize);
    mold::Request back{};
    ASSERT_EQ(mold::parse_request(buf, back), mold::RequestStatus::Ok);
    EXPECT_EQ(back.session, r.session);
    EXPECT_EQ(back.sequence, r.sequence);
    EXPECT_EQ(back.count, r.count);
  }
}

TEST(MoldCodec, RandomBytesNeverCrashAndAnythingAcceptedReencodesIdentically) {
  lob::testkit::Rng rng{0x46555A5A};  // "FUZZ"
  int accepted = 0;
  for (int i = 0; i < 20'000; ++i) {
    // Half the inputs start from a valid packet and get a few bytes flipped,
    // so that the deeper branches are reached; the rest are pure noise.
    Buf in;
    if (rng.percent(50)) {
      Buf buf(mold::kDefaultMaxPacketSize);
      mold::PacketBuilder b{buf, session("SESSION001"), static_cast<std::uint64_t>(rng.uniform(1, 1000))};
      const auto n = rng.uniform(1, 8);
      for (std::int64_t k = 0; k < n; ++k) {
        (void)b.add(Buf(static_cast<std::size_t>(rng.uniform(1, 40)), static_cast<std::uint8_t>(k)));
      }
      in = bytes_of(b.finish());
      const auto flips = rng.uniform(0, 3);
      for (std::int64_t k = 0; k < flips; ++k) {
        in[static_cast<std::size_t>(rng.uniform(0, static_cast<std::int64_t>(in.size()) - 1))] ^=
            static_cast<std::uint8_t>(1U << rng.uniform(0, 7));
      }
      if (rng.percent(20)) {
        in.resize(static_cast<std::size_t>(rng.uniform(0, static_cast<std::int64_t>(in.size()))));
      }
    } else {
      in.resize(static_cast<std::size_t>(rng.uniform(0, 80)));
      for (auto& byte : in) {
        byte = static_cast<std::uint8_t>(rng.uniform(0, 255));
      }
    }

    const Parsed p = parse(in);
    if (p.status == mold::Status::Ok) {
      ++accepted;
      Buf buf(in.size());
      mold::PacketBuilder b{buf, p.header.session, p.header.sequence};
      for (const Buf& m : p.msgs) {
        ASSERT_EQ(b.add(m), mold::PacketBuilder::Add::Ok);
      }
      ASSERT_EQ(bytes_of(b.finish()), in) << "input " << i;
    }
    mold::Request r{};
    if (mold::parse_request(in, r) == mold::RequestStatus::Ok) {
      Buf buf(mold::kRequestSize);
      ASSERT_EQ(mold::encode_request(buf, r), mold::kRequestSize);
      ASSERT_EQ(buf, in) << "input " << i;
    }
  }
  EXPECT_GT(accepted, 1000);  // the mutated half must reach the accepting path
}

}  // namespace
