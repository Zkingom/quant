// L4.2: SoupBinTCP 3.00 framing and sans-IO session state machines.
// docs/design/L4_DESIGN.md §2.5. One named test per state transition
// (ClientState_Event_Outcome / ServerState_Event_Outcome); time is a plain
// number, no sockets, no sleeping.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "lob/soup/client.hpp"
#include "lob/soup/server.hpp"
#include "lob/soup/soup.hpp"
#include "lob/testkit/rng.hpp"

namespace {

namespace soup = lob::soup;
using soup::ClientState;
using soup::CloseReason;
using soup::ServerState;
using Buf = std::vector<std::uint8_t>;

constexpr std::uint64_t kSec = 1'000'000'000ULL;

Buf bytes(std::string_view s) { return Buf(s.begin(), s.end()); }

/// One logical packet: length, type, payload.
Buf pkt(char t, std::string_view payload = {}) {
  const std::size_t len = payload.size() + 1;
  Buf b{static_cast<std::uint8_t>(len >> 8U), static_cast<std::uint8_t>(len & 0xffU), static_cast<std::uint8_t>(t)};
  b.insert(b.end(), payload.begin(), payload.end());
  return b;
}

Buf cat(std::initializer_list<Buf> parts) {
  Buf out;
  for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
  return out;
}

struct Packet {
  char type;
  std::string payload;
  bool operator==(const Packet&) const = default;
};

std::vector<Packet> packets(soup::Bytes b) {
  std::vector<Packet> out;
  soup::Deframer d;
  EXPECT_EQ(d.feed(b, [&](char t, soup::Bytes p) {
    out.push_back(Packet{t, std::string(p.begin(), p.end())});
    return true;
  }),
            soup::FrameError::None);
  return out;
}

std::vector<Packet> drain(soup::ClientSession& c) {
  auto p = packets(c.out().pending());
  c.out().consume(c.out().pending().size());
  return p;
}

std::vector<Packet> drain(soup::ServerConnection& s, std::uint64_t now) {
  auto p = packets(s.pending());
  s.consume(s.pending().size(), now);
  return p;
}

// "Login Accepted" payload: session left-padded to 10, sequence left-padded to 20.
std::string accepted(std::string_view session, std::uint64_t seq) {
  const std::string n = std::to_string(seq);
  return std::string(10 - session.size(), ' ') + std::string(session) + std::string(20 - n.size(), ' ') + n;
}

// "Login Request" payload: user 6, password 10, session 10 (right-padded), sequence 20.
std::string login(std::string_view user, std::string_view pw, std::string_view session, std::uint64_t seq) {
  const std::string n = std::to_string(seq);
  return std::string(user) + std::string(6 - user.size(), ' ') + std::string(pw) + std::string(10 - pw.size(), ' ') +
         std::string(session) + std::string(10 - session.size(), ' ') + std::string(20 - n.size(), ' ') + n;
}

soup::ClientConfig client_cfg() {
  soup::ClientConfig c;
  c.user = "ZKQT";
  c.password = "secret";
  return c;
}

soup::ServerConfig server_cfg() {
  soup::ServerConfig s;
  s.user = "zkqt";       // credentials are case-insensitive
  s.password = "SECRET";
  return s;
}

const soup::SessionId kSession = soup::field<10>("D201901300");

struct Messages {
  std::vector<std::pair<std::uint64_t, std::string>> got;
  void operator()(std::uint64_t seq, soup::Bytes p) { got.emplace_back(seq, std::string(p.begin(), p.end())); }
};

// --- Framing -------------------------------------------------------------------

TEST(SoupFraming, PacketsSplitAtEveryByteBoundaryEqualTheWholeStream) {
  const Buf stream = cat({pkt('+', "hello"), pkt('A', accepted("S1", 1)), pkt('S', "abc"), pkt('H'), pkt('S', ""),
                          pkt('S', std::string(200, 'x')), pkt('Z')});
  const auto whole = packets(stream);
  ASSERT_EQ(whole.size(), 7U);
  for (std::size_t cut1 = 0; cut1 <= stream.size(); ++cut1) {
    for (std::size_t cut2 = cut1; cut2 <= stream.size(); cut2 += 7) {  // two cuts: three chunks
      soup::Deframer d;
      std::vector<Packet> got;
      auto f = [&](char t, soup::Bytes p) {
        got.push_back(Packet{t, std::string(p.begin(), p.end())});
        return true;
      };
      ASSERT_EQ(d.feed({stream.data(), cut1}, f), soup::FrameError::None);
      ASSERT_EQ(d.feed({stream.data() + cut1, cut2 - cut1}, f), soup::FrameError::None);
      ASSERT_EQ(d.feed({stream.data() + cut2, stream.size() - cut2}, f), soup::FrameError::None);
      ASSERT_EQ(got, whole) << cut1 << " " << cut2;
    }
  }
}

TEST(SoupFraming, OneByteAtATimeEqualsTheWholeStream) {
  const Buf stream = cat({pkt('S', "x"), pkt('+', std::string(4000, 'd')), pkt('S', "y")});
  soup::Deframer d;
  std::vector<Packet> got;
  for (const std::uint8_t b : stream) {
    ASSERT_EQ(d.feed({&b, 1}, [&](char t, soup::Bytes p) {
      got.push_back(Packet{t, std::string(p.begin(), p.end())});
      return true;
    }),
              soup::FrameError::None);
  }
  EXPECT_EQ(got, packets(stream));
}

TEST(SoupFraming, ZeroLengthIsAnError) {
  soup::Deframer d;
  const Buf b{0x00, 0x00, 'S'};
  EXPECT_EQ(d.feed(b, [](char, soup::Bytes) { return true; }), soup::FrameError::ZeroLength);
  EXPECT_EQ(d.feed(pkt('S', "x"), [](char, soup::Bytes) { return true; }), soup::FrameError::ZeroLength);  // stays failed
}

TEST(SoupFraming, DataPacketLongerThanTheLimitIsAnError) {
  soup::Deframer d;
  EXPECT_EQ(d.feed(pkt('S', std::string(soup::kMaxPacket - 1, 'x')), [](char, soup::Bytes) { return true; }),
            soup::FrameError::None);
  EXPECT_EQ(d.feed(pkt('S', std::string(soup::kMaxPacket, 'x')), [](char, soup::Bytes) { return true; }),
            soup::FrameError::TooLong);
}

TEST(SoupFraming, DebugPacketsMayBeLongerUpToTheirOwnLimit) {
  soup::Deframer d;
  EXPECT_EQ(d.feed(pkt('+', std::string(soup::kMaxDebugPacket - 1, 'd')), [](char, soup::Bytes) { return true; }),
            soup::FrameError::None);
  soup::Deframer d2;
  const Buf huge{0xff, 0xff};  // known too long from the length alone
  EXPECT_EQ(d2.feed(huge, [](char, soup::Bytes) { return true; }), soup::FrameError::TooLong);
}

TEST(SoupFraming, OutBufferRefusesInsteadOfGrowingAndCompacts) {
  soup::OutBuffer out(10);
  EXPECT_TRUE(out.append_packet('S', bytes("abcd")));  // 7 bytes
  EXPECT_FALSE(out.append_packet('S', bytes("ab")));   // 5 more do not fit
  out.consume(7);
  EXPECT_TRUE(out.empty());
  EXPECT_TRUE(out.append_packet('S', bytes("abcdefg")));  // exactly 10
  out.consume(4);                           // "00 08 S a" sent; "bcdefg" still waiting
  EXPECT_TRUE(out.append_packet('H', {}));  // needs compaction: 6 + 3 <= 10
  const auto p = out.pending();
  EXPECT_EQ(Buf(p.begin(), p.end()), cat({bytes("bcdefg"), Buf{0x00, 0x01, 'H'}}));
  EXPECT_FALSE(out.append_packet('H', {}));  // 12 > 10
}

TEST(SoupFraming, LoginFieldsRoundTripAndRejectMalformedNumbers) {
  soup::OutBuffer out(128);
  ASSERT_TRUE(soup::put_login_request(out, "ZKQT", "secret", kSession, 1234));
  const auto p = packets(out.pending());
  ASSERT_EQ(p.size(), 1U);
  EXPECT_EQ(p[0].type, 'L');
  EXPECT_EQ(p[0].payload, login("ZKQT", "secret", "D201901300", 1234));  // spec layout, by hand
  soup::LoginRequest r;
  ASSERT_TRUE(soup::parse_login_request({reinterpret_cast<const std::uint8_t*>(p[0].payload.data()), p[0].payload.size()}, r));
  EXPECT_EQ(r.sequence, 1234U);
  soup::LoginAccepted a;
  for (const std::string& bad : {std::string(30, ' '), std::string(10, ' ') + std::string(19, ' ') + "x",
                                std::string(10, ' ') + "12 3" + std::string(16, ' '),
                                std::string(10, ' ') + "99999999999999999999" /* > 2^64 */}) {
    EXPECT_FALSE(soup::parse_login_accepted({reinterpret_cast<const std::uint8_t*>(bad.data()), bad.size()}, a)) << bad;
  }
  const std::string ok = accepted("S1", 18446744073709551615ULL);
  ASSERT_TRUE(soup::parse_login_accepted({reinterpret_cast<const std::uint8_t*>(ok.data()), ok.size()}, a));
  EXPECT_EQ(a.sequence, 18446744073709551615ULL);
}

// --- Client transitions ----------------------------------------------------------

TEST(SoupClient, Disconnected_Connect_SendsLoginRequestAndGoesLoginSent) {
  soup::ClientSession c(client_cfg());
  EXPECT_EQ(c.state(), ClientState::Disconnected);
  c.on_connected(0);
  EXPECT_EQ(c.state(), ClientState::LoginSent);
  // Blank session, sequence 1 (the whole day), byte for byte.
  EXPECT_EQ(drain(c), (std::vector<Packet>{{'L', login("ZKQT", "secret", "", 1)}}));
}

TEST(SoupClient, LoginSent_LoginAccepted_GoesLoggedInAndAdoptsSession) {
  soup::ClientSession c(client_cfg());
  c.on_connected(0);
  Messages m;
  c.on_bytes(pkt('A', accepted("D201901300", 1)), 1, m);
  EXPECT_EQ(c.state(), ClientState::LoggedIn);
  EXPECT_EQ(std::string(c.session().data(), 10), "D201901300");
  EXPECT_EQ(c.next_sequence(), 1U);
  EXPECT_EQ(c.logins(), 1U);
}

TEST(SoupClient, LoginSent_LoginRejected_GoesRejectedTerminal) {
  soup::ClientSession c(client_cfg());
  c.on_connected(0);
  Messages m;
  c.on_bytes(pkt('J', "A"), 1, m);
  EXPECT_EQ(c.state(), ClientState::Rejected);
  EXPECT_EQ(c.close_reason(), CloseReason::LoginRejected);
  EXPECT_TRUE(c.wants_close());
  c.on_disconnected();
  c.on_connected(2);  // no automatic retry from a rejection
  EXPECT_EQ(c.state(), ClientState::Rejected);
}

TEST(SoupClient, LoginSent_DataBeforeLoginAccepted_IsProtocolError) {
  soup::ClientSession c(client_cfg());
  c.on_connected(0);
  Messages m;
  c.on_bytes(pkt('S', "x"), 1, m);
  EXPECT_TRUE(m.got.empty());
  EXPECT_EQ(c.close_reason(), CloseReason::ProtocolError);
  EXPECT_EQ(c.state(), ClientState::Disconnected);
  EXPECT_TRUE(c.wants_close());
}

TEST(SoupClient, LoginSent_NoAnswerWithinTimeout_ClosesToDisconnected) {
  soup::ClientSession c(client_cfg());
  c.on_connected(0);
  c.on_timer(15 * kSec - 1);
  EXPECT_EQ(c.state(), ClientState::LoginSent);
  c.on_timer(15 * kSec);
  EXPECT_EQ(c.state(), ClientState::Disconnected);
  EXPECT_EQ(c.close_reason(), CloseReason::Timeout);
}

TEST(SoupClient, LoginSent_AcceptedAtOtherSequence_IsSequenceMismatchTerminal) {
  for (const std::uint64_t seq : {std::uint64_t{0}, std::uint64_t{2}}) {
    soup::ClientSession c(client_cfg());
    c.on_connected(0);
    Messages m;
    c.on_bytes(pkt('A', accepted("D201901300", seq)), 1, m);  // requested 1
    EXPECT_EQ(c.state(), ClientState::Failed) << seq;
    EXPECT_EQ(c.close_reason(), CloseReason::SequenceMismatch);
    c.on_disconnected();
    c.on_connected(2);
    EXPECT_EQ(c.state(), ClientState::Failed);
  }
}

TEST(SoupClient, LoginSent_AcceptedForAnotherSession_IsMismatch) {
  auto cfg = client_cfg();
  cfg.session = kSession;
  soup::ClientSession c(cfg);
  c.on_connected(0);
  Messages m;
  c.on_bytes(pkt('A', accepted("D201901301", 1)), 1, m);
  EXPECT_EQ(c.state(), ClientState::Failed);
}

TEST(SoupClient, LoginSent_MalformedLoginAccepted_IsProtocolError) {
  soup::ClientSession c(client_cfg());
  c.on_connected(0);
  Messages m;
  c.on_bytes(pkt('A', "short"), 1, m);
  EXPECT_EQ(c.close_reason(), CloseReason::ProtocolError);
}

TEST(SoupClient, LoggedIn_SequencedData_DeliveredInOrderWithImplicitSequence) {
  soup::ClientSession c(client_cfg());
  c.on_connected(0);
  Messages m;
  c.on_bytes(cat({pkt('A', accepted("S", 1)), pkt('S', "one"), pkt('S', "two"), pkt('S', "three")}), 1, m);
  ASSERT_EQ(m.got.size(), 3U);
  EXPECT_EQ(m.got[0], (std::pair<std::uint64_t, std::string>{1, "one"}));
  EXPECT_EQ(m.got[2], (std::pair<std::uint64_t, std::string>{3, "three"}));
  EXPECT_EQ(c.next_sequence(), 4U);
}

TEST(SoupClient, LoggedIn_ServerHeartbeatAndDebug_OnlyRefreshTheLink) {
  soup::ClientSession c(client_cfg());
  c.on_connected(0);
  Messages m;
  c.on_bytes(pkt('A', accepted("S", 1)), 0, m);
  c.on_bytes(pkt('H'), 10 * kSec, m);
  c.on_bytes(pkt('+', "debug text"), 20 * kSec, m);
  c.on_timer(34 * kSec);  // 14 s since the last packet: still alive
  EXPECT_EQ(c.state(), ClientState::LoggedIn);
  EXPECT_TRUE(m.got.empty());
  EXPECT_EQ(c.next_sequence(), 1U);
}

TEST(SoupClient, LoggedIn_IdleOneSecond_SendsClientHeartbeat) {
  soup::ClientSession c(client_cfg());
  c.on_connected(0);
  Messages m;
  c.on_bytes(pkt('A', accepted("S", 1)), 0, m);
  drain(c);
  c.on_timer(kSec - 1);
  EXPECT_TRUE(drain(c).empty());
  EXPECT_EQ(c.next_deadline(), kSec);
  c.on_timer(kSec);
  EXPECT_EQ(drain(c), (std::vector<Packet>{{'R', ""}}));
  // Data sent counts as "sent something": no heartbeat for another second.
  const Buf d = bytes("order");
  ASSERT_TRUE(c.send(d, kSec + 500'000'000));
  drain(c);
  c.on_timer(2 * kSec);
  EXPECT_TRUE(drain(c).empty());
}

TEST(SoupClient, LoggedIn_NothingReceivedFor15s_TimesOutToDisconnected) {
  soup::ClientSession c(client_cfg());
  c.on_connected(0);
  Messages m;
  c.on_bytes(pkt('A', accepted("S", 1)), 0, m);
  c.on_timer(15 * kSec);
  EXPECT_EQ(c.state(), ClientState::Disconnected);
  EXPECT_EQ(c.close_reason(), CloseReason::Timeout);
}

TEST(SoupClient, LoggedIn_EndOfSession_GoesSessionEndedTerminal) {
  soup::ClientSession c(client_cfg());
  c.on_connected(0);
  Messages m;
  c.on_bytes(cat({pkt('A', accepted("S", 1)), pkt('S', "last"), pkt('Z'), pkt('S', "after")}), 1, m);
  EXPECT_EQ(m.got.size(), 1U);  // nothing after End of Session is processed
  EXPECT_EQ(c.state(), ClientState::SessionEnded);
  EXPECT_EQ(c.close_reason(), CloseReason::EndOfSession);
}

TEST(SoupClient, LoggedIn_Logout_SendsLogoutRequestAndGoesLogoutSent) {
  soup::ClientSession c(client_cfg());
  c.on_connected(0);
  Messages m;
  c.on_bytes(pkt('A', accepted("S", 1)), 0, m);
  drain(c);
  c.logout(1);
  EXPECT_EQ(c.state(), ClientState::LogoutSent);
  EXPECT_EQ(drain(c), (std::vector<Packet>{{'O', ""}}));
  const Buf d = bytes("late");
  EXPECT_FALSE(c.send(d, 2));  // no data after logout
  c.on_disconnected();         // the server closes the connection
  EXPECT_EQ(c.state(), ClientState::Disconnected);
}

TEST(SoupClient, LoggedIn_Disconnect_KeepsSessionAndNextSequenceForReconnect) {
  soup::ClientSession c(client_cfg());
  c.on_connected(0);
  Messages m;
  c.on_bytes(cat({pkt('A', accepted("D201901300", 1)), pkt('S', "a"), pkt('S', "b")}), 1, m);
  c.on_disconnected();
  EXPECT_EQ(c.state(), ClientState::Disconnected);
  EXPECT_EQ(c.close_reason(), CloseReason::PeerClosed);
  c.on_connected(2);
  EXPECT_EQ(c.state(), ClientState::LoginSent);
  // The left-padded session as received, and the next expected sequence number.
  EXPECT_EQ(drain(c), (std::vector<Packet>{{'L', login("ZKQT", "secret", "D201901300", 3)}}));
  c.on_bytes(cat({pkt('A', accepted("D201901300", 3)), pkt('S', "c")}), 3, m);
  ASSERT_EQ(m.got.size(), 3U);
  EXPECT_EQ(m.got[2], (std::pair<std::uint64_t, std::string>{3, "c"}));
}

TEST(SoupClient, LoggedIn_UnexpectedPacketTypes_AreProtocolErrors) {
  for (const Buf& bad : {pkt('L', login("a", "b", "", 1)), pkt('U', "x"), pkt('A', accepted("S", 1)), pkt('J', "A"),
                         pkt('H', "payload"), pkt('Q')}) {
    soup::ClientSession c(client_cfg());
    c.on_connected(0);
    Messages m;
    c.on_bytes(pkt('A', accepted("S", 1)), 0, m);
    c.on_bytes(bad, 1, m);
    EXPECT_EQ(c.close_reason(), CloseReason::ProtocolError) << static_cast<char>(bad[2]);
    EXPECT_EQ(c.state(), ClientState::Disconnected);
  }
}

TEST(SoupClient, LoggedIn_FramingError_IsProtocolError) {
  soup::ClientSession c(client_cfg());
  c.on_connected(0);
  Messages m;
  c.on_bytes(pkt('A', accepted("S", 1)), 0, m);
  const Buf zero{0, 0};
  c.on_bytes(zero, 1, m);
  EXPECT_EQ(c.close_reason(), CloseReason::ProtocolError);
}

TEST(SoupClient, SendOutsideLoggedInOrIntoFullBuffer_IsRefused) {
  auto cfg = client_cfg();
  cfg.out_capacity = 60;
  soup::ClientSession c(cfg);
  const Buf d(30, 'x');
  EXPECT_FALSE(c.send(d, 0));  // Disconnected
  c.on_connected(0);
  EXPECT_FALSE(c.send(d, 0));  // LoginSent
  Messages m;
  c.on_bytes(pkt('A', accepted("S", 1)), 0, m);
  drain(c);
  EXPECT_TRUE(c.send(d, 1));   // 33 bytes
  EXPECT_FALSE(c.send(d, 1));  // 66 > 60: the caller keeps the message
}

// --- Server connection transitions -------------------------------------------------

struct Srv {
  soup::SessionLog log{kSession, 1000, 1U << 16};
  soup::ServerConfig cfg = server_cfg();
  std::vector<std::string> in;
  void append(std::string_view s) { ASSERT_NE(log.append({reinterpret_cast<const std::uint8_t*>(s.data()), s.size()}), 0U); }
  auto sink() {
    return [this](soup::Bytes p) { in.emplace_back(p.begin(), p.end()); };
  }
};

TEST(SoupServer, AwaitLogin_ValidLogin_SendsLoginAcceptedAndGoesLoggedIn) {
  Srv s;
  soup::ServerConnection c(s.cfg, s.log, 0);
  EXPECT_EQ(c.state(), ServerState::AwaitLogin);
  c.on_bytes(pkt('L', login("ZKQT", "secret", "", 1)), 1, s.sink());
  EXPECT_EQ(c.state(), ServerState::LoggedIn);
  EXPECT_EQ(drain(c, 1), (std::vector<Packet>{{'A', accepted("D201901300", 1)}}));
}

TEST(SoupServer, AwaitLogin_WrongPassword_RejectsNotAuthorizedAndCloses) {
  Srv s;
  soup::ServerConnection c(s.cfg, s.log, 0);
  c.on_bytes(pkt('L', login("ZKQT", "wrong", "", 1)), 1, s.sink());
  EXPECT_EQ(drain(c, 1), (std::vector<Packet>{{'J', "A"}}));
  EXPECT_EQ(c.state(), ServerState::Closed);
  EXPECT_EQ(c.close_reason(), CloseReason::LoginRejected);
}

TEST(SoupServer, AwaitLogin_UnknownSession_RejectsSessionNotAvailable) {
  Srv s;
  soup::ServerConnection c(s.cfg, s.log, 0);
  c.on_bytes(pkt('L', login("ZKQT", "secret", "D201901299", 1)), 1, s.sink());
  EXPECT_EQ(drain(c, 1), (std::vector<Packet>{{'J', "S"}}));
}

TEST(SoupServer, AwaitLogin_EndedSession_RejectsSessionNotAvailable) {
  Srv s;
  s.log.end();
  soup::ServerConnection c(s.cfg, s.log, 0);
  c.on_bytes(pkt('L', login("ZKQT", "secret", "", 1)), 1, s.sink());
  EXPECT_EQ(drain(c, 1), (std::vector<Packet>{{'J', "S"}}));
}

TEST(SoupServer, AwaitLogin_AccountAlreadyConnected_RejectsSessionNotAvailable) {
  Srv s;
  soup::ServerConnection c(s.cfg, s.log, 0);
  c.set_login_allowed(false);
  c.on_bytes(pkt('L', login("ZKQT", "secret", "", 1)), 1, s.sink());
  EXPECT_EQ(drain(c, 1), (std::vector<Packet>{{'J', "S"}}));
}

TEST(SoupServer, AwaitLogin_CredentialsAreCaseInsensitive) {
  Srv s;
  soup::ServerConnection c(s.cfg, s.log, 0);
  c.on_bytes(pkt('L', login("zKqT", "SeCrEt", "D201901300", 1)), 1, s.sink());
  EXPECT_EQ(c.state(), ServerState::LoggedIn);
}

TEST(SoupServer, AwaitLogin_FirstPacketNotLogin_ClosesWithProtocolError) {
  Srv s;
  soup::ServerConnection c(s.cfg, s.log, 0);
  c.on_bytes(pkt('+', "hi"), 0, s.sink());  // debug is ignored even before login
  EXPECT_EQ(c.state(), ServerState::AwaitLogin);
  c.on_bytes(pkt('U', "order"), 1, s.sink());
  EXPECT_EQ(c.state(), ServerState::Closed);
  EXPECT_EQ(c.close_reason(), CloseReason::ProtocolError);
  EXPECT_TRUE(s.in.empty());
}

TEST(SoupServer, AwaitLogin_MalformedLoginRequest_ClosesWithProtocolError) {
  Srv s;
  soup::ServerConnection c(s.cfg, s.log, 0);
  c.on_bytes(pkt('L', "too short"), 1, s.sink());
  EXPECT_EQ(c.close_reason(), CloseReason::ProtocolError);
}

TEST(SoupServer, AwaitLogin_NoLoginWithin30s_Closes) {
  Srv s;
  soup::ServerConnection c(s.cfg, s.log, 0);
  EXPECT_EQ(c.next_deadline(), 30 * kSec);
  c.on_timer(30 * kSec - 1);
  EXPECT_EQ(c.state(), ServerState::AwaitLogin);
  c.on_timer(30 * kSec);
  EXPECT_EQ(c.close_reason(), CloseReason::LoginTimeout);
}

TEST(SoupServer, LoggedIn_ReplaysFromTheRequestedSequence) {
  Srv s;
  for (const char* m : {"m1", "m2", "m3", "m4"}) s.append(m);
  soup::ServerConnection c(s.cfg, s.log, 0);
  c.on_bytes(pkt('L', login("ZKQT", "secret", "D201901300", 3)), 1, s.sink());
  EXPECT_EQ(drain(c, 1), (std::vector<Packet>{{'A', accepted("D201901300", 3)}, {'S', "m3"}, {'S', "m4"}}));
  s.append("m5");
  c.pump(2);
  EXPECT_EQ(drain(c, 2), (std::vector<Packet>{{'S', "m5"}}));
}

TEST(SoupServer, LoggedIn_RequestedZero_OnlyNewMessages) {
  Srv s;
  s.append("old");
  soup::ServerConnection c(s.cfg, s.log, 0);
  c.on_bytes(pkt('L', login("ZKQT", "secret", "", 0)), 1, s.sink());
  EXPECT_EQ(drain(c, 1), (std::vector<Packet>{{'A', accepted("D201901300", 2)}}));
}

TEST(SoupServer, LoggedIn_RequestedBeyondTheEnd_StartsAtTheEnd) {
  Srv s;
  s.append("only");
  soup::ServerConnection c(s.cfg, s.log, 0);
  c.on_bytes(pkt('L', login("ZKQT", "secret", "", 9)), 1, s.sink());
  EXPECT_EQ(drain(c, 1), (std::vector<Packet>{{'A', accepted("D201901300", 2)}}));
}

TEST(SoupServer, LoggedIn_UnsequencedData_DeliveredInOrder) {
  Srv s;
  soup::ServerConnection c(s.cfg, s.log, 0);
  c.on_bytes(cat({pkt('L', login("ZKQT", "secret", "", 1)), pkt('U', "o1"), pkt('R'), pkt('+', "dbg"), pkt('U', "o2")}),
             1, s.sink());
  EXPECT_EQ(s.in, (std::vector<std::string>{"o1", "o2"}));
}

TEST(SoupServer, LoggedIn_LogoutRequest_ClosesImmediately) {
  Srv s;
  soup::ServerConnection c(s.cfg, s.log, 0);
  c.on_bytes(cat({pkt('L', login("ZKQT", "secret", "", 1)), pkt('O'), pkt('U', "after")}), 1, s.sink());
  EXPECT_EQ(c.state(), ServerState::Closed);
  EXPECT_EQ(c.close_reason(), CloseReason::Logout);
  EXPECT_TRUE(s.in.empty());
}

TEST(SoupServer, LoggedIn_IdleOneSecond_SendsServerHeartbeat) {
  Srv s;
  soup::ServerConnection c(s.cfg, s.log, 0);
  c.on_bytes(pkt('L', login("ZKQT", "secret", "", 1)), 0, s.sink());
  drain(c, 0);
  c.on_timer(kSec - 1);
  EXPECT_TRUE(drain(c, kSec - 1).empty());
  c.on_timer(kSec);
  EXPECT_EQ(drain(c, kSec), (std::vector<Packet>{{'H', ""}}));
}

TEST(SoupServer, LoggedIn_NothingReceivedFor15s_Closes) {
  Srv s;
  soup::ServerConnection c(s.cfg, s.log, 0);
  c.on_bytes(pkt('L', login("ZKQT", "secret", "", 1)), 0, s.sink());
  drain(c, 0);  // the client reads: not a slow consumer
  c.on_bytes(pkt('R'), 10 * kSec, s.sink());
  c.on_timer(25 * kSec - 1);
  EXPECT_EQ(c.state(), ServerState::LoggedIn);
  c.on_timer(25 * kSec);
  EXPECT_EQ(c.close_reason(), CloseReason::Timeout);
}

TEST(SoupServer, LoggedIn_SessionEnded_SendsEverythingThenEndOfSessionAndCloses) {
  Srv s;
  s.append("m1");
  soup::ServerConnection c(s.cfg, s.log, 0);
  c.on_bytes(pkt('L', login("ZKQT", "secret", "", 1)), 0, s.sink());
  s.append("m2");
  s.log.end();
  EXPECT_EQ(s.log.append(bytes("m3")), 0U);  // nothing after the end
  c.pump(1);
  EXPECT_EQ(drain(c, 1),
            (std::vector<Packet>{{'A', accepted("D201901300", 1)}, {'S', "m1"}, {'S', "m2"}, {'Z', ""}}));
  EXPECT_EQ(c.close_reason(), CloseReason::EndOfSession);
}

TEST(SoupServer, LoggedIn_ClientSendsServerPacketTypes_IsProtocolError) {
  for (const Buf& bad : {pkt('S', "x"), pkt('A', accepted("S", 1)), pkt('L', login("ZKQT", "secret", "", 1)),
                         pkt('R', "payload"), pkt('O', "payload"), pkt('Q')}) {
    Srv s;
    soup::ServerConnection c(s.cfg, s.log, 0);
    c.on_bytes(pkt('L', login("ZKQT", "secret", "", 1)), 0, s.sink());
    c.on_bytes(bad, 1, s.sink());
    EXPECT_EQ(c.close_reason(), CloseReason::ProtocolError) << static_cast<char>(bad[2]);
  }
}

TEST(SoupServer, LoggedIn_SendBufferFull_PumpResumesAfterConsume) {
  Srv s;
  s.cfg.out_capacity = 64;
  for (int i = 0; i < 10; ++i) s.append("message-" + std::to_string(i));  // 9-byte payloads: 12 bytes per packet
  soup::ServerConnection c(s.cfg, s.log, 0);
  c.on_bytes(pkt('L', login("ZKQT", "secret", "", 1)), 0, s.sink());
  std::vector<Packet> got;
  for (int round = 0; round < 10 && got.size() < 11; ++round) {
    for (auto& p : drain(c, 0)) got.push_back(p);
    c.pump(0);
  }
  ASSERT_EQ(got.size(), 11U);  // Login Accepted + 10 messages, in order, never more than 64 bytes buffered
  EXPECT_EQ(got[10].payload, "message-9");
  EXPECT_EQ(c.next_to_send(), 11U);
}

TEST(SoupServer, LoggedIn_PeerStopsReading_ClosedAsSlowConsumer) {
  Srv s;
  s.cfg.out_capacity = 64;
  for (int i = 0; i < 10; ++i) s.append("message-" + std::to_string(i));
  soup::ServerConnection c(s.cfg, s.log, 0);
  c.on_bytes(pkt('L', login("ZKQT", "secret", "", 1)), 0, s.sink());
  for (std::uint64_t t = 1; t < 15; ++t) c.on_bytes(pkt('R'), t * kSec, s.sink());  // alive, but never reads
  c.on_timer(15 * kSec);
  EXPECT_EQ(c.close_reason(), CloseReason::SlowConsumer);
}

TEST(SoupServer, SessionLogRefusesWhenFull) {
  soup::SessionLog log(kSession, 2, 100);
  EXPECT_EQ(log.append(bytes("a")), 1U);
  EXPECT_EQ(log.append(bytes("b")), 2U);
  EXPECT_EQ(log.append(bytes("c")), 0U);
  soup::SessionLog small(kSession, 10, 3);
  EXPECT_EQ(small.append(bytes("abcd")), 0U);
  EXPECT_EQ(small.refused(), 1U);
}

// --- Robustness: arbitrary bytes never crash either side -----------------------------

TEST(SoupRobustness, RandomBytesNeverCrashAndEndInALegalState) {
  for (std::uint64_t seed = 1; seed <= 4; ++seed) {
    lob::testkit::Rng rng(seed);
    for (int iter = 0; iter < 2000; ++iter) {
      // Valid prefix (login exchange) half of the time, so the logged-in paths are reached.
      Buf to_server = rng.percent(50) ? pkt('L', login("ZKQT", "secret", "", 1)) : Buf{};
      Buf to_client = rng.percent(50) ? pkt('A', accepted("S", 1)) : Buf{};
      const auto n = rng.uniform(0, 80);
      for (std::int64_t i = 0; i < n; ++i) {
        const auto b = static_cast<std::uint8_t>(rng.percent(30) ? rng.uniform(0, 3) : rng.uniform(0, 255));
        to_server.push_back(b);
        to_client.push_back(b);
      }
      Srv s;
      soup::ServerConnection server(s.cfg, s.log, 0);
      server.on_bytes(to_server, 1, s.sink());
      server.on_timer(2);
      EXPECT_TRUE(server.state() == ServerState::AwaitLogin || server.state() == ServerState::LoggedIn ||
                  server.state() == ServerState::Closed);
      soup::ClientSession client(client_cfg());
      client.on_connected(0);
      Messages m;
      client.on_bytes(to_client, 1, m);
      client.on_timer(2);
      // Every delivered message advanced the implicit sequence counter, and nothing else did.
      EXPECT_EQ(m.got.size() + 1, client.next_sequence());
      for (std::size_t i = 0; i < m.got.size(); ++i) EXPECT_EQ(m.got[i].first, i + 1);
    }
  }
}

// --- Client <-> server, sans-IO, with arbitrary chunking and reconnects --------------

TEST(SoupEndToEnd, EveryMessageArrivesExactlyOnceInOrderAcrossReconnects) {
  for (std::uint64_t seed = 1; seed <= 20; ++seed) {
    lob::testkit::Rng rng(seed);
    soup::SessionLog log(kSession, 5000, 1U << 20);
    auto scfg = server_cfg();
    scfg.out_capacity = static_cast<std::size_t>(rng.uniform(64, 512));
    auto ccfg = client_cfg();
    soup::ClientSession client(ccfg);
    Messages got;
    std::vector<std::string> received_by_server;
    std::uint64_t now = 0;
    int produced = 0;
    for (int conn = 0; conn < 6; ++conn) {
      soup::ServerConnection server(scfg, log, now);
      client.on_connected(now);
      Buf c2s;
      for (int step = 0; step < 200; ++step) {
        now += static_cast<std::uint64_t>(rng.uniform(0, 300'000'000));
        if (rng.percent(40) && produced < 3000) {
          const std::string m = "msg" + std::to_string(++produced);
          ASSERT_NE(log.append({reinterpret_cast<const std::uint8_t*>(m.data()), m.size()}), 0U);
          server.pump(now);
        }
        if (client.state() == ClientState::LoggedIn && rng.percent(20)) {
          const std::string o = "order" + std::to_string(step);
          (void)client.send({reinterpret_cast<const std::uint8_t*>(o.data()), o.size()}, now);
        }
        // client -> server in a random-size chunk
        {
          const auto p = client.out().pending();
          const auto n = static_cast<std::size_t>(rng.uniform(0, static_cast<std::int64_t>(p.size())));
          c2s.assign(p.begin(), p.begin() + static_cast<std::ptrdiff_t>(n));
          client.out().consume(n);
          server.on_bytes(c2s, now, [&](soup::Bytes b) { received_by_server.emplace_back(b.begin(), b.end()); });
        }
        // server -> client in a random-size chunk
        {
          const auto p = server.pending();
          const auto n = static_cast<std::size_t>(rng.uniform(0, static_cast<std::int64_t>(p.size())));
          const Buf chunk(p.begin(), p.begin() + static_cast<std::ptrdiff_t>(n));
          server.consume(n, now);
          client.on_bytes(chunk, now, got);
        }
        server.on_timer(now);
        client.on_timer(now);
        ASSERT_NE(client.state(), ClientState::Failed) << "seed " << seed;
        if (server.wants_close() || client.wants_close()) break;
      }
      // The connection drops (unsent bytes in flight are lost).
      server.on_disconnected();
      client.on_disconnected();
    }
    // Final connection: drain everything.
    soup::ServerConnection server(scfg, log, now);
    client.on_connected(now);
    for (int i = 0; i < 100000 && client.next_sequence() < log.next_sequence(); ++i) {
      const auto cp = client.out().pending();
      const Buf c2s(cp.begin(), cp.end());
      client.out().consume(cp.size());
      server.on_bytes(c2s, now, [](soup::Bytes) {});
      server.pump(now);
      const auto p = server.pending();
      const Buf chunk(p.begin(), p.end());
      server.consume(p.size(), now);
      client.on_bytes(chunk, now, got);
    }
    ASSERT_EQ(got.got.size(), static_cast<std::size_t>(produced)) << "seed " << seed;
    for (std::size_t i = 0; i < got.got.size(); ++i) {
      ASSERT_EQ(got.got[i].first, i + 1) << "seed " << seed;
      ASSERT_EQ(got.got[i].second, "msg" + std::to_string(i + 1)) << "seed " << seed;
    }
    EXPECT_GE(client.logins(), 2U) << "seed " << seed;
  }
}

}  // namespace
