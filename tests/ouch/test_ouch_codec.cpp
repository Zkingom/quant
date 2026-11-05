// L4.1: OUCH 4.2 subset codec. docs/design/L4_DESIGN.md §1.
//
//   - one hand-written literal per message type, field by field from the
//     specification's offsets (comments); decoded field values and the
//     encoder's output are both pinned to it, independently;
//   - every truncation and extension of every literal -> BadLength / Empty;
//   - non-subset spec types are Skipped only at their exact length; every
//     other type byte is UnknownType, per direction;
//   - every validated field: an invalid value -> InvalidField, boundary and
//     unusual-but-valid values -> Decoded;
//   - deterministic mutational fuzzing (fixed seeds, every preset): decode
//     never crashes and a Decoded payload re-encodes to the same bytes.

#include "lob/ouch/ouch.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "lob/testkit/rng.hpp"

namespace {

namespace ouch = lob::ouch;
using ouch::DecodeStatus;
using Buf = std::vector<std::uint8_t>;

Buf hex(std::string_view s) {
  Buf out;
  int nibble = -1;
  for (const char ch : s) {
    int v = -1;
    if (ch >= '0' && ch <= '9') v = ch - '0';
    else if (ch >= 'a' && ch <= 'f') v = ch - 'a' + 10;
    else if (ch >= 'A' && ch <= 'F') v = ch - 'A' + 10;
    if (v < 0) continue;
    if (nibble < 0) {
      nibble = v;
    } else {
      out.push_back(static_cast<std::uint8_t>(nibble * 16 + v));
      nibble = -1;
    }
  }
  return out;
}

std::string str(const auto& a) { return std::string(a.data(), a.size()); }

// --- Literals -----------------------------------------------------------------
// Token "ABC123" = 41 42 43 31 32 33 + 8 spaces; "AAPL    " = 4141504C20202020;
// $150.0000 = 1 500 000 = 0016E360; $149.9900 = 0016E2FC; TIF 99 999 (system
// hours) = 0001869F; 09:30:00 = 34 200 000 000 000 ns = 00001F1ACED9F000.

// O Enter Order (49): type 0 | token 1:14 | side 15 | shares 16:4 | stock 20:8 |
// price 28:4 | TIF 32:4 | firm 36:4 | display 40 | capacity 41 | ISO 42 |
// min qty 43:4 | cross type 47 | customer type 48
const char* const kEnter =
    "4F 4142433132332020202020202020 42 00000064 4141504C20202020 0016E360 0001869F 20202020"
    " 41 50 4E 00000000 4E 20";
// U Replace Order (47): type 0 | existing 1:14 | replacement 15:14 | shares 29:4 |
// price 33:4 | TIF 37:4 | display 41 | ISO 42 | min qty 43:4
const char* const kReplace =
    "55 4142433132332020202020202020 4142433132342020202020202020 000000C8 0016E2FC 0001869F"
    " 59 4E 00000000";
// X Cancel Order (19): type 0 | token 1:14 | intended shares 15:4 (0 = all)
const char* const kCancel = "58 4142433132342020202020202020 00000000";

// S System Event (10): type 0 | timestamp 1:8 | event code 9
const char* const kSystemEvent = "53 00001F1ACED9F000 53";
// A Accepted (66): type 0 | ts 1:8 | token 9:14 | side 23 | shares 24:4 | stock 28:8 |
// price 36:4 | TIF 40:4 | firm 44:4 | display 48 | order ref 49:8 | capacity 57 |
// ISO 58 | min qty 59:4 | cross type 63 | order state 64 | BBO weight 65
const char* const kAccepted =
    "41 00001F1ACED9F3E8 4142433132332020202020202020 42 00000064 4141504C20202020 0016E360"
    " 0001869F 5A4B5154 41 00000000000003E9 50 4E 00000000 4E 4C 20";
// U Replaced (80): type 0 | ts 1:8 | replacement token 9:14 | side 23 | shares 24:4 |
// stock 28:8 | price 36:4 | TIF 40:4 | firm 44:4 | display 48 | order ref 49:8 |
// capacity 57 | ISO 58 | min qty 59:4 | cross type 63 | order state 64 |
// previous token 65:14 | BBO weight 79
const char* const kReplaced =
    "55 00001F1ACED9FBB8 4142433132342020202020202020 42 00000064 4141504C20202020 0016E2FC"
    " 0001869F 5A4B5154 59 00000000000003EA 50 4E 00000000 4E 4C 4142433132332020202020202020 20";
// C Canceled (28): type 0 | ts 1:8 | token 9:14 | decrement 23:4 | reason 27
const char* const kCanceled = "43 00001F1ACED9FFA0 4142433132342020202020202020 00000064 55";
// E Executed (40): type 0 | ts 1:8 | token 9:14 | shares 23:4 | price 27:4 |
// liquidity 31 | match number 32:8
const char* const kExecuted =
    "45 00001F1ACED9F7D0 4142433132332020202020202020 00000064 0016E360 41 0000000000001E61";
// J Rejected (24): type 0 | ts 1:8 | token 9:14 | reason 23 ('x' port rate)
const char* const kRejected = "4A 00001F1ACEDA0388 4142433132352020202020202020 78";

struct Sample {
  const char* name;
  bool inbound;
  const char* hex;
};
const Sample kSamples[] = {
    {"EnterOrder", true, kEnter},       {"ReplaceOrder", true, kReplace}, {"CancelOrder", true, kCancel},
    {"SystemEvent", false, kSystemEvent}, {"Accepted", false, kAccepted}, {"Replaced", false, kReplaced},
    {"Canceled", false, kCanceled},     {"Executed", false, kExecuted},   {"Rejected", false, kRejected},
};

DecodeStatus decode(bool inbound, const Buf& b) {
  if (inbound) {
    ouch::Inbound m;
    return ouch::decode_inbound(b, m);
  }
  ouch::Outbound m;
  return ouch::decode_outbound(b, m);
}

/// Re-encodes whatever decoded; empty when it did not decode.
Buf reencode(bool inbound, const Buf& b) {
  Buf out(ouch::kMaxMessageSize + 8, 0xAA);
  std::size_t n = 0;
  if (inbound) {
    ouch::Inbound m;
    if (ouch::decode_inbound(b, m) != DecodeStatus::Decoded) return {};
    n = ouch::encode(m, out);
  } else {
    ouch::Outbound m;
    if (ouch::decode_outbound(b, m) != DecodeStatus::Decoded) return {};
    n = ouch::encode(m, out);
  }
  out.resize(n);
  return out;
}

template <class M>
M in_ok(const char* h) {
  ouch::Inbound m;
  EXPECT_EQ(ouch::decode_inbound(hex(h), m), DecodeStatus::Decoded);
  EXPECT_TRUE(std::holds_alternative<M>(m));
  return std::holds_alternative<M>(m) ? std::get<M>(m) : M{};
}

template <class M>
M out_ok(const char* h) {
  ouch::Outbound m;
  EXPECT_EQ(ouch::decode_outbound(hex(h), m), DecodeStatus::Decoded);
  EXPECT_TRUE(std::holds_alternative<M>(m));
  return std::holds_alternative<M>(m) ? std::get<M>(m) : M{};
}

template <class M>
Buf enc(const M& m) {
  Buf out(ouch::kMaxMessageSize);
  out.resize(ouch::encode(m, out));
  return out;
}

Buf with(const char* h, std::size_t off, const std::vector<std::uint8_t>& bytes) {
  Buf b = hex(h);
  std::size_t i = off;
  for (const auto v : bytes) b.at(i++) = v;
  return b;
}

// --- Literals: decoded fields and the encoder, each against the spec ------------

TEST(OuchCodec, EnterOrderLiteral) {
  const auto m = in_ok<ouch::EnterOrder>(kEnter);
  EXPECT_EQ(str(m.token), "ABC123        ");
  EXPECT_EQ(m.side, ouch::Side::Buy);
  EXPECT_EQ(m.shares, 100U);
  EXPECT_EQ(str(m.stock), "AAPL    ");
  EXPECT_EQ(m.price, 1'500'000U);
  EXPECT_EQ(m.time_in_force, ouch::kTifSystemHours);
  EXPECT_EQ(str(m.firm), "    ");
  EXPECT_EQ(m.display, 'A');
  EXPECT_EQ(m.capacity, 'P');
  EXPECT_EQ(m.iso, 'N');
  EXPECT_EQ(m.min_qty, 0U);
  EXPECT_EQ(m.cross_type, 'N');
  EXPECT_EQ(m.customer_type, ' ');
  // The encoder, from a struct built by hand (defaults: A P N 0 N ' ').
  ouch::EnterOrder e;
  e.token = ouch::make_token("ABC123");
  e.side = ouch::Side::Buy;
  e.shares = 100;
  e.stock = ouch::make_symbol("AAPL");
  e.price = 1'500'000;
  e.time_in_force = ouch::kTifSystemHours;
  e.firm = ouch::alpha<4>("");
  EXPECT_EQ(enc(e), hex(kEnter));
}

TEST(OuchCodec, ReplaceOrderLiteral) {
  const auto m = in_ok<ouch::ReplaceOrder>(kReplace);
  EXPECT_EQ(str(m.existing), "ABC123        ");
  EXPECT_EQ(str(m.replacement), "ABC124        ");
  EXPECT_EQ(m.shares, 200U);
  EXPECT_EQ(m.price, 1'499'900U);
  EXPECT_EQ(m.time_in_force, 99'999U);
  EXPECT_EQ(m.display, 'Y');
  EXPECT_EQ(m.iso, 'N');
  EXPECT_EQ(m.min_qty, 0U);
  ouch::ReplaceOrder r{ouch::make_token("ABC123"), ouch::make_token("ABC124"), 200, 1'499'900, 99'999, 'Y', 'N', 0};
  EXPECT_EQ(enc(r), hex(kReplace));
}

TEST(OuchCodec, CancelOrderLiteral) {
  const auto m = in_ok<ouch::CancelOrder>(kCancel);
  EXPECT_EQ(str(m.token), "ABC124        ");
  EXPECT_EQ(m.shares, 0U);  // intended order size 0 = cancel everything
  EXPECT_EQ(enc(ouch::CancelOrder{ouch::make_token("ABC124"), 0}), hex(kCancel));
}

TEST(OuchCodec, SystemEventLiteral) {
  const auto m = out_ok<ouch::SystemEvent>(kSystemEvent);
  EXPECT_EQ(m.timestamp, 34'200'000'000'000ULL);
  EXPECT_EQ(m.event_code, 'S');
  EXPECT_EQ(enc(ouch::SystemEvent{34'200'000'000'000ULL, 'S'}), hex(kSystemEvent));
}

TEST(OuchCodec, AcceptedLiteral) {
  const auto m = out_ok<ouch::Accepted>(kAccepted);
  EXPECT_EQ(m.timestamp, 34'200'000'001'000ULL);
  EXPECT_EQ(str(m.token), "ABC123        ");
  EXPECT_EQ(m.side, ouch::Side::Buy);
  EXPECT_EQ(m.shares, 100U);
  EXPECT_EQ(str(m.stock), "AAPL    ");
  EXPECT_EQ(m.price, 1'500'000U);
  EXPECT_EQ(m.time_in_force, 99'999U);
  EXPECT_EQ(str(m.firm), "ZKQT");
  EXPECT_EQ(m.display, 'A');
  EXPECT_EQ(m.order_ref, 1001U);
  EXPECT_EQ(m.capacity, 'P');
  EXPECT_EQ(m.iso, 'N');
  EXPECT_EQ(m.min_qty, 0U);
  EXPECT_EQ(m.cross_type, 'N');
  EXPECT_EQ(m.order_state, 'L');
  EXPECT_EQ(m.bbo_weight, ' ');
  ouch::Accepted a;
  a.timestamp = 34'200'000'001'000ULL;
  a.token = ouch::make_token("ABC123");
  a.shares = 100;
  a.stock = ouch::make_symbol("AAPL");
  a.price = 1'500'000;
  a.time_in_force = 99'999;
  a.firm = ouch::alpha<4>("ZKQT");
  a.order_ref = 1001;
  EXPECT_EQ(enc(a), hex(kAccepted));
}

TEST(OuchCodec, ReplacedLiteral) {
  const auto m = out_ok<ouch::Replaced>(kReplaced);
  EXPECT_EQ(m.timestamp, 34'200'000'003'000ULL);
  EXPECT_EQ(str(m.replacement), "ABC124        ");
  EXPECT_EQ(m.shares, 100U);
  EXPECT_EQ(m.price, 1'499'900U);
  EXPECT_EQ(m.display, 'Y');
  EXPECT_EQ(m.order_ref, 1002U);
  EXPECT_EQ(m.order_state, 'L');
  EXPECT_EQ(str(m.previous), "ABC123        ");
  EXPECT_EQ(m.bbo_weight, ' ');
  ouch::Replaced r;
  r.timestamp = 34'200'000'003'000ULL;
  r.replacement = ouch::make_token("ABC124");
  r.shares = 100;
  r.stock = ouch::make_symbol("AAPL");
  r.price = 1'499'900;
  r.time_in_force = 99'999;
  r.firm = ouch::alpha<4>("ZKQT");
  r.display = 'Y';
  r.order_ref = 1002;
  r.previous = ouch::make_token("ABC123");
  EXPECT_EQ(enc(r), hex(kReplaced));
}

TEST(OuchCodec, CanceledLiteral) {
  const auto m = out_ok<ouch::Canceled>(kCanceled);
  EXPECT_EQ(m.timestamp, 34'200'000'004'000ULL);
  EXPECT_EQ(str(m.token), "ABC124        ");
  EXPECT_EQ(m.decrement, 100U);
  EXPECT_EQ(m.reason, ouch::reason::kUserCancel);
  EXPECT_EQ(enc(ouch::Canceled{34'200'000'004'000ULL, ouch::make_token("ABC124"), 100, 'U'}), hex(kCanceled));
}

TEST(OuchCodec, ExecutedLiteral) {
  const auto m = out_ok<ouch::Executed>(kExecuted);
  EXPECT_EQ(m.timestamp, 34'200'000'002'000ULL);
  EXPECT_EQ(str(m.token), "ABC123        ");
  EXPECT_EQ(m.shares, 100U);
  EXPECT_EQ(m.price, 1'500'000U);
  EXPECT_EQ(m.liquidity, 'A');
  EXPECT_EQ(m.match_number, 7777U);
  EXPECT_EQ(enc(ouch::Executed{34'200'000'002'000ULL, ouch::make_token("ABC123"), 100, 1'500'000, 'A', 7777}),
            hex(kExecuted));
}

TEST(OuchCodec, RejectedLiteral) {
  const auto m = out_ok<ouch::Rejected>(kRejected);
  EXPECT_EQ(m.timestamp, 34'200'000'005'000ULL);
  EXPECT_EQ(str(m.token), "ABC125        ");
  EXPECT_EQ(m.reason, ouch::reason::kRejectPortRate);
  EXPECT_EQ(enc(ouch::Rejected{34'200'000'005'000ULL, ouch::make_token("ABC125"), 'x'}), hex(kRejected));
}

TEST(OuchCodec, EveryLiteralHasTheSpecLengthAndRoundTrips) {
  for (const auto& s : kSamples) {
    const Buf b = hex(s.hex);
    const char type = static_cast<char>(b.at(0));
    EXPECT_EQ(b.size(), s.inbound ? ouch::inbound_length(type) : ouch::outbound_length(type)) << s.name;
    EXPECT_EQ(reencode(s.inbound, b), b) << s.name;
  }
}

// --- Lengths ---------------------------------------------------------------------

TEST(OuchCodec, EveryTruncationOfEveryLiteralIsRejected) {
  for (const auto& s : kSamples) {
    const Buf b = hex(s.hex);
    for (std::size_t n = 0; n < b.size(); ++n) {
      const Buf cut(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(n));
      EXPECT_EQ(decode(s.inbound, cut), n == 0 ? DecodeStatus::Empty : DecodeStatus::BadLength)
          << s.name << " cut to " << n;
    }
  }
}

TEST(OuchCodec, OneExtraByteIsRejected) {
  for (const auto& s : kSamples) {
    Buf b = hex(s.hex);
    b.push_back(0x20);
    EXPECT_EQ(decode(s.inbound, b), DecodeStatus::BadLength) << s.name;
  }
}

TEST(OuchCodec, NonSubsetSpecTypesAreSkippedOnlyAtTheirExactLength) {
  struct T {
    bool inbound;
    char type;
    std::size_t len;
  };
  // Lengths from the specification's field tables (last offset + length).
  for (const T t : {T{true, 'M', 20}, T{false, 'B', 32}, T{false, 'G', 45}, T{false, 'D', 38}, T{false, 'P', 23},
                    T{false, 'I', 23}, T{false, 'T', 36}, T{false, 'M', 28}}) {
    Buf b(t.len, 0x20);
    b[0] = static_cast<std::uint8_t>(t.type);
    EXPECT_EQ(decode(t.inbound, b), DecodeStatus::Skipped) << t.type;
    b.push_back(0x20);
    EXPECT_EQ(decode(t.inbound, b), DecodeStatus::BadLength) << t.type;
    b.resize(t.len - 1);
    EXPECT_EQ(decode(t.inbound, b), DecodeStatus::BadLength) << t.type;
  }
}

TEST(OuchCodec, EveryUndefinedTypeByteIsRejectedPerDirection) {
  const std::set<int> in{'O', 'U', 'X', 'M'};
  const std::set<int> out{'S', 'A', 'U', 'C', 'E', 'J', 'B', 'G', 'D', 'P', 'I', 'T', 'M'};
  for (int t = 0; t < 256; ++t) {
    const Buf b(1, static_cast<std::uint8_t>(t));  // defined types: BadLength, others: UnknownType
    const DecodeStatus si = decode(true, b);
    const DecodeStatus so = decode(false, b);
    EXPECT_EQ(si == DecodeStatus::UnknownType, in.count(t) == 0) << t;
    EXPECT_EQ(so == DecodeStatus::UnknownType, out.count(t) == 0) << t;
  }
  // The same byte means different messages in the two directions.
  EXPECT_EQ(decode(true, hex(kAccepted)), DecodeStatus::UnknownType);  // 'A' is outbound only
  EXPECT_EQ(decode(false, hex(kEnter)), DecodeStatus::UnknownType);    // 'O' is inbound only
  EXPECT_EQ(decode(true, hex(kReplaced)), DecodeStatus::BadLength);    // 'U' inbound is 47 bytes
  EXPECT_EQ(decode(false, hex(kReplace)), DecodeStatus::BadLength);    // 'U' outbound is 80 bytes
}

TEST(OuchCodec, FailedDecodeLeavesOutputUnchanged) {
  ouch::Inbound in = ouch::CancelOrder{ouch::make_token("KEEP"), 7};
  EXPECT_EQ(ouch::decode_inbound(with(kEnter, 15, {'X'}), in), DecodeStatus::InvalidField);
  ASSERT_TRUE(std::holds_alternative<ouch::CancelOrder>(in));
  EXPECT_EQ(std::get<ouch::CancelOrder>(in).shares, 7U);
  ouch::Outbound out = ouch::SystemEvent{1, 'E'};
  EXPECT_EQ(ouch::decode_outbound(Buf{}, out), DecodeStatus::Empty);
  ASSERT_TRUE(std::holds_alternative<ouch::SystemEvent>(out));
  EXPECT_EQ(std::get<ouch::SystemEvent>(out).timestamp, 1U);
}

TEST(OuchCodec, EncodeIntoAShortBufferWritesNothing) {
  for (const auto& s : kSamples) {
    const Buf b = hex(s.hex);
    Buf small(b.size() - 1, 0xEE);
    std::size_t n = 0;
    if (s.inbound) {
      ouch::Inbound m;
      ASSERT_EQ(ouch::decode_inbound(b, m), DecodeStatus::Decoded);
      n = ouch::encode(m, small);
    } else {
      ouch::Outbound m;
      ASSERT_EQ(ouch::decode_outbound(b, m), DecodeStatus::Decoded);
      n = ouch::encode(m, small);
    }
    EXPECT_EQ(n, 0U) << s.name;
    EXPECT_EQ(small, Buf(b.size() - 1, 0xEE)) << s.name;
  }
}

// --- Fields ----------------------------------------------------------------------

struct Mutation {
  const char* what;
  bool inbound;
  const char* base;
  std::size_t offset;
  std::vector<std::uint8_t> bytes;
};

TEST(OuchCodec, InvalidFieldValuesAreRejected) {
  const std::vector<Mutation> bad = {
      // Enter Order
      {"token '-'", true, kEnter, 1, {'-'}},
      {"token NUL", true, kEnter, 14, {0x00}},
      {"side X", true, kEnter, 15, {'X'}},
      {"side b", true, kEnter, 15, {'b'}},
      {"shares 0", true, kEnter, 16, {0, 0, 0, 0}},
      {"shares 1 000 000", true, kEnter, 16, {0x00, 0x0F, 0x42, 0x40}},
      {"stock non-printable", true, kEnter, 23, {0x01}},
      {"price max+1", true, kEnter, 28, {0x77, 0x35, 0x93, 0x9D}},
      {"price between market values", true, kEnter, 28, {0x77, 0x35, 0x94, 0x01}},
      {"price 0x80000000", true, kEnter, 28, {0x80, 0x00, 0x00, 0x00}},
      {"firm DEL", true, kEnter, 36, {0x7F}},
      {"display Z (outbound only)", true, kEnter, 40, {'Z'}},
      {"capacity NUL", true, kEnter, 41, {0x00}},
      {"ISO X", true, kEnter, 42, {'X'}},
      {"cross type R (not in the current table)", true, kEnter, 47, {'R'}},
      {"customer type Y", true, kEnter, 48, {'Y'}},
      // Replace Order
      {"existing token '_'", true, kReplace, 1, {'_'}},
      {"replacement token '.'", true, kReplace, 28, {'.'}},
      {"replace shares 0", true, kReplace, 29, {0, 0, 0, 0}},
      {"replace price max+1", true, kReplace, 33, {0x77, 0x35, 0x93, 0x9D}},
      {"replace display Z", true, kReplace, 41, {'Z'}},
      {"replace ISO n", true, kReplace, 42, {'n'}},
      // Cancel Order
      {"cancel token 0xFF", true, kCancel, 3, {0xFF}},
      // System Event
      {"timestamp 24h", false, kSystemEvent, 1, {0x00, 0x00, 0x4E, 0x94, 0x91, 0x4F, 0x00, 0x00}},
      {"event code O (ITCH, not OUCH)", false, kSystemEvent, 9, {'O'}},
      // Accepted
      {"accepted timestamp max u64", false, kAccepted, 1, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}},
      {"accepted token '/'", false, kAccepted, 9, {'/'}},
      {"accepted side 0", false, kAccepted, 23, {'0'}},
      {"accepted stock 0x80", false, kAccepted, 28, {0x80}},
      {"accepted price max+1", false, kAccepted, 36, {0x77, 0x35, 0x93, 0x9D}},
      {"accepted firm LF", false, kAccepted, 44, {0x0A}},
      {"accepted display x", false, kAccepted, 48, {'x'}},
      {"accepted capacity LF", false, kAccepted, 57, {0x0A}},
      {"accepted ISO Z", false, kAccepted, 58, {'Z'}},
      {"accepted cross type n", false, kAccepted, 63, {'n'}},
      {"accepted order state X", false, kAccepted, 64, {'X'}},
      {"accepted BBO weight 4", false, kAccepted, 65, {'4'}},
      // Replaced
      {"replaced replacement token '!'", false, kReplaced, 9, {'!'}},
      {"replaced side space", false, kReplaced, 23, {' '}},
      {"replaced order state l", false, kReplaced, 64, {'l'}},
      {"replaced previous token '#'", false, kReplaced, 65, {'#'}},
      {"replaced BBO weight 9", false, kReplaced, 79, {'9'}},
      // Canceled
      {"canceled reason lower-case", false, kCanceled, 27, {'u'}},
      {"canceled reason digit", false, kCanceled, 27, {'1'}},
      {"canceled token '@'", false, kCanceled, 9, {'@'}},
      // Executed
      {"executed price max+1", false, kExecuted, 27, {0x77, 0x35, 0x93, 0x9D}},
      {"executed price market value", false, kExecuted, 27, {0x7F, 0xFF, 0xFF, 0xFF}},
      {"executed liquidity '#'", false, kExecuted, 31, {'#'}},
      // Rejected
      {"rejected reason digit", false, kRejected, 23, {'1'}},
      {"rejected reason space", false, kRejected, 23, {' '}},
      {"rejected token '~'", false, kRejected, 22, {'~'}},
  };
  for (const auto& m : bad) {
    const Buf b = with(m.base, m.offset, m.bytes);
    EXPECT_EQ(decode(m.inbound, b), DecodeStatus::InvalidField) << m.what;
  }
}

TEST(OuchCodec, BoundaryAndUnusualValidValuesDecode) {
  const std::vector<Mutation> good = {
      {"shares 999 999", true, kEnter, 16, {0x00, 0x0F, 0x42, 0x3F}},
      {"price max", true, kEnter, 28, {0x77, 0x35, 0x93, 0x9C}},
      {"price 0 (refused by the server, not the codec)", true, kEnter, 28, {0, 0, 0, 0}},
      {"market price $200,000", true, kEnter, 28, {0x77, 0x35, 0x94, 0x00}},
      {"market price 0x7FFFFFFF", true, kEnter, 28, {0x7F, 0xFF, 0xFF, 0xFF}},
      {"TIF IOC", true, kEnter, 32, {0, 0, 0, 0}},
      {"TIF above 99 999 (spec: treated as system hours)", true, kEnter, 32, {0xFF, 0xFF, 0xFF, 0xFF}},
      {"side sell short", true, kEnter, 15, {'T'}},
      {"side sell short exempt", true, kEnter, 15, {'E'}},
      {"lower-case token (tokens are case sensitive)", true, kEnter, 1, {'a', 'b', 'c'}},
      {"display midpoint peg lower-case m", true, kEnter, 40, {'m'}},
      {"ISO trade-at y", true, kEnter, 42, {'y'}},
      {"cross type closing", true, kEnter, 47, {'C'}},
      {"customer type R", true, kEnter, 48, {'R'}},
      {"min qty max", true, kEnter, 43, {0xFF, 0xFF, 0xFF, 0xFF}},
      {"cancel intended size max u32", true, kCancel, 15, {0xFF, 0xFF, 0xFF, 0xFF}},
      {"timestamp 24h - 1ns", false, kSystemEvent, 1, {0x00, 0x00, 0x4E, 0x94, 0x91, 0x4E, 0xFF, 0xFF}},
      {"end of day", false, kSystemEvent, 9, {'E'}},
      {"accepted display Z", false, kAccepted, 48, {'Z'}},
      {"accepted dead", false, kAccepted, 64, {'D'}},
      {"accepted BBO weight S", false, kAccepted, 65, {'S'}},
      {"accepted market price", false, kAccepted, 36, {0x7F, 0xFF, 0xFF, 0xFF}},
      {"replaced display B", false, kReplaced, 48, {'B'}},
      {"canceled reason any capital (Q)", false, kCanceled, 27, {'Q'}},
      {"canceled system cancel Z", false, kCanceled, 27, {'Z'}},
      {"executed liquidity digit", false, kExecuted, 31, {'7'}},
      {"executed liquidity lower-case", false, kExecuted, 31, {'m'}},
      {"executed match max", false, kExecuted, 32, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}},
      {"rejected reason upper-case", false, kRejected, 23, {'H'}},
  };
  for (const auto& m : good) {
    const Buf b = with(m.base, m.offset, m.bytes);
    EXPECT_EQ(decode(m.inbound, b), DecodeStatus::Decoded) << m.what;
    EXPECT_EQ(reencode(m.inbound, b), b) << m.what;
  }
}

TEST(OuchCodec, MarketPriceHelpers) {
  EXPECT_TRUE(ouch::is_market_price(ouch::kMarketPrice));
  EXPECT_TRUE(ouch::is_market_price(ouch::kMarketPriceMaxInt));
  EXPECT_FALSE(ouch::is_market_price(ouch::kMaxPrice));
  EXPECT_TRUE(ouch::valid_order_price(ouch::kMaxPrice));
  EXPECT_FALSE(ouch::valid_order_price(ouch::kMaxPrice + 1));
  EXPECT_EQ(ouch::kMaxPrice, 0x7735939CU);  // the hex value printed in the specification
}

TEST(OuchCodec, StatusNamesAreDistinct) {
  std::set<std::string_view> names;
  for (const auto s : {DecodeStatus::Decoded, DecodeStatus::Skipped, DecodeStatus::Empty, DecodeStatus::UnknownType,
                       DecodeStatus::BadLength, DecodeStatus::InvalidField}) {
    names.insert(ouch::to_string(s));
  }
  EXPECT_EQ(names.size(), 6U);
}

// --- Golden file (also the libFuzzer seed corpus, scripts/fuzz_ouch.sh) ------------

TEST(OuchCodec, GoldenSamplesMatchTheLiterals) {
  std::ifstream f(std::string(LOB_GOLDEN_DIR) + "/ouch/samples.txt");
  ASSERT_TRUE(f.good());
  std::string line;
  std::size_t n = 0;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream is(line);
    std::string dir, name;
    is >> dir >> name;
    std::string rest;
    std::getline(is, rest);
    bool found = false;
    for (const auto& s : kSamples) {
      if (name == s.name) {
        found = true;
        EXPECT_EQ(dir == "in", s.inbound) << name;
        EXPECT_EQ(hex(rest), hex(s.hex)) << name;
      }
    }
    EXPECT_TRUE(found) << name;
    ++n;
  }
  EXPECT_EQ(n, std::size(kSamples));
}

// --- Deterministic mutational fuzzing --------------------------------------------

TEST(OuchCodec, MutationFuzzNeverCrashesAndDecodedRoundTrips) {
  std::uint64_t decoded = 0, total = 0;
  for (std::uint64_t seed = 1; seed <= 8; ++seed) {
    lob::testkit::Rng rng(seed);
    for (int iter = 0; iter < 5000; ++iter) {
      const Sample& s = kSamples[static_cast<std::size_t>(rng.uniform(0, static_cast<std::int64_t>(std::size(kSamples)) - 1))];
      Buf b = hex(s.hex);
      const auto edits = rng.uniform(1, 4);
      for (std::int64_t e = 0; e < edits; ++e) {
        switch (rng.uniform(0, 4)) {
          case 0:  // random byte anywhere
            if (!b.empty()) b[static_cast<std::size_t>(rng.uniform(0, static_cast<std::int64_t>(b.size()) - 1))] =
                static_cast<std::uint8_t>(rng.uniform(0, 255));
            break;
          case 1:  // printable byte (keeps many fields valid -> reaches deeper checks)
            if (!b.empty()) b[static_cast<std::size_t>(rng.uniform(0, static_cast<std::int64_t>(b.size()) - 1))] =
                static_cast<std::uint8_t>(rng.uniform(0x20, 0x7e));
            break;
          case 2:  // truncate
            b.resize(static_cast<std::size_t>(rng.uniform(0, static_cast<std::int64_t>(b.size()))));
            break;
          case 3:  // extend
            b.push_back(static_cast<std::uint8_t>(rng.uniform(0, 255)));
            break;
          default:  // change the type byte to another defined type
            if (!b.empty()) b[0] = static_cast<std::uint8_t>(std::string_view("OUXMSACEJBGDPITM")[static_cast<std::size_t>(rng.uniform(0, 15))]);
            break;
        }
      }
      for (const bool inbound : {true, false}) {
        ++total;
        const Buf r = reencode(inbound, b);
        if (!r.empty()) {
          ++decoded;
          EXPECT_EQ(r, b) << "seed " << seed << " iter " << iter;
        }
      }
    }
  }
  // The corpus must reach the deep checks, not only the length check.
  EXPECT_GT(decoded, total / 50);
}

}  // namespace
