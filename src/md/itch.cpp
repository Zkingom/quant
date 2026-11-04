#include "lob/md/itch.hpp"

#include <algorithm>

namespace lob::md::itch {

namespace {

struct Reader {
  const std::uint8_t* p;

  [[nodiscard]] char alpha(std::size_t off) const noexcept { return static_cast<char>(p[off]); }
  [[nodiscard]] std::uint16_t u16(std::size_t off) const noexcept { return be16(p + off); }
  [[nodiscard]] std::uint32_t u32(std::size_t off) const noexcept { return be32(p + off); }
  [[nodiscard]] std::uint64_t u48(std::size_t off) const noexcept { return be48(p + off); }
  [[nodiscard]] std::uint64_t u64(std::size_t off) const noexcept { return be64(p + off); }
  template <std::size_t N>
  [[nodiscard]] std::array<char, N> chars(std::size_t off) const noexcept {
    std::array<char, N> a{};
    for (std::size_t i = 0; i < N; ++i) {
      a[i] = static_cast<char>(p[off + i]);
    }
    return a;
  }
  [[nodiscard]] Header header() const noexcept {
    return Header{alpha(0), u16(1), u16(3), u48(5)};
  }
};

bool valid_side(char c) noexcept { return c == 'B' || c == 'S'; }

bool valid_trading_state(char c) noexcept { return c == 'H' || c == 'P' || c == 'Q' || c == 'T'; }

bool valid_event_code(char c) noexcept {
  switch (c) {
    case 'O': case 'S': case 'Q': case 'M': case 'E': case 'C':
      return true;
    default:
      return false;
  }
}

}  // namespace

std::string_view to_string(DecodeStatus s) noexcept {
  switch (s) {
    case DecodeStatus::Decoded: return "DECODED";
    case DecodeStatus::Skipped: return "SKIPPED";
    case DecodeStatus::Empty: return "EMPTY";
    case DecodeStatus::UnknownType: return "UNKNOWN_TYPE";
    case DecodeStatus::BadLength: return "BAD_LENGTH";
    case DecodeStatus::InvalidField: return "INVALID_FIELD";
  }
  return "?";
}

std::size_t spec_length(char type) noexcept {
  // ITCH 5.0 §4 (lengths include the 11-byte header).
  switch (type) {
    case 'S': return 12;  // System Event
    case 'R': return 39;  // Stock Directory
    case 'H': return 25;  // Stock Trading Action
    case 'Y': return 20;  // Reg SHO Short Sale Price Test Restricted Indicator
    case 'L': return 26;  // Market Participant Position
    case 'V': return 35;  // MWCB Decline Level
    case 'W': return 12;  // MWCB Status
    case 'K': return 28;  // IPO Quoting Period Update
    case 'J': return 35;  // LULD Auction Collar
    case 'h': return 21;  // Operational Halt
    case 'A': return 36;  // Add Order (no MPID)
    case 'F': return 40;  // Add Order with MPID Attribution
    case 'E': return 31;  // Order Executed
    case 'C': return 36;  // Order Executed with Price
    case 'X': return 23;  // Order Cancel
    case 'D': return 19;  // Order Delete
    case 'U': return 35;  // Order Replace
    case 'P': return 44;  // Trade (non-cross)
    case 'Q': return 40;  // Cross Trade
    case 'B': return 19;  // Broken Trade
    case 'I': return 50;  // NOII
    case 'N': return 20;  // Retail Price Improvement Indicator
    case 'O': return 48;  // Direct Listing with Capital Raise Price Discovery
    default: return 0;
  }
}

DecodeStatus decode(Bytes payload, Message& out) noexcept {
  if (payload.empty()) {
    return DecodeStatus::Empty;
  }
  const char type = static_cast<char>(payload[0]);
  const std::size_t expected = spec_length(type);
  if (expected == 0) {
    return DecodeStatus::UnknownType;
  }
  if (payload.size() != expected) {
    return DecodeStatus::BadLength;
  }
  const Reader r{payload.data()};
  const Header h = r.header();
  if (h.timestamp >= kNanosPerDay) {
    return DecodeStatus::InvalidField;
  }

  switch (type) {
    case 'S': {
      const char code = r.alpha(11);
      if (!valid_event_code(code)) {
        return DecodeStatus::InvalidField;
      }
      out = SystemEvent{h, code};
      return DecodeStatus::Decoded;
    }
    case 'R': {
      StockDirectory m{};
      m.header = h;
      m.stock = r.chars<8>(11);
      m.market_category = r.alpha(19);
      m.financial_status = r.alpha(20);
      m.round_lot_size = r.u32(21);
      m.round_lots_only = r.alpha(25);
      m.issue_classification = r.alpha(26);
      m.issue_subtype = r.chars<2>(27);
      m.authenticity = r.alpha(29);
      m.short_sale_threshold = r.alpha(30);
      m.ipo_flag = r.alpha(31);
      m.luld_reference_price_tier = r.alpha(32);
      m.etp_flag = r.alpha(33);
      m.etp_leverage_factor = r.u32(34);
      m.inverse_indicator = r.alpha(38);
      out = m;
      return DecodeStatus::Decoded;
    }
    case 'A':
    case 'F': {
      const char side = r.alpha(19);
      if (!valid_side(side)) {
        return DecodeStatus::InvalidField;
      }
      AddOrder m{};
      m.header = h;
      m.order_ref = r.u64(11);
      m.side = static_cast<Side>(side);
      m.shares = r.u32(20);
      m.stock = r.chars<8>(24);
      m.price = r.u32(32);
      if (type == 'F') {
        m.attribution = r.chars<4>(36);
        m.has_mpid = true;
      }
      out = m;
      return DecodeStatus::Decoded;
    }
    case 'E':
      out = OrderExecuted{h, r.u64(11), r.u32(19), r.u64(23)};
      return DecodeStatus::Decoded;
    case 'C': {
      const char printable = r.alpha(31);
      if (printable != 'Y' && printable != 'N') {
        return DecodeStatus::InvalidField;
      }
      out = OrderExecutedWithPrice{h, r.u64(11), r.u32(19), r.u64(23), printable, r.u32(32)};
      return DecodeStatus::Decoded;
    }
    case 'X':
      out = OrderCancel{h, r.u64(11), r.u32(19)};
      return DecodeStatus::Decoded;
    case 'D':
      out = OrderDelete{h, r.u64(11)};
      return DecodeStatus::Decoded;
    case 'U':
      out = OrderReplace{h, r.u64(11), r.u64(19), r.u32(27), r.u32(31)};
      return DecodeStatus::Decoded;
    case 'H': {
      const char state = r.alpha(19);
      if (!valid_trading_state(state)) {
        return DecodeStatus::InvalidField;
      }
      out = TradingAction{h, r.chars<8>(11), state, r.alpha(20), r.chars<4>(21)};
      return DecodeStatus::Decoded;
    }
    default:
      return DecodeStatus::Skipped;
  }
}

FrameStatus FrameCursor::next(Bytes& payload) noexcept {
  const std::size_t remaining = buffer_.size() - offset_;
  if (remaining == 0) {
    return FrameStatus::End;
  }
  if (remaining < 2) {
    return FrameStatus::Truncated;
  }
  const std::size_t length = be16(buffer_.data() + offset_);
  if (length == 0) {
    return FrameStatus::ZeroLength;
  }
  if (remaining - 2 < length) {
    return FrameStatus::Truncated;
  }
  payload = buffer_.subspan(offset_ + 2, length);
  offset_ += 2 + length;
  return FrameStatus::Frame;
}

}  // namespace lob::md::itch
