#include "lob/ouch/ouch.hpp"

#include <algorithm>

namespace lob::ouch {
namespace {

// --- Character classes ----------------------------------------------------
constexpr bool is_upper(char c) noexcept { return c >= 'A' && c <= 'Z'; }
constexpr bool is_lower(char c) noexcept { return c >= 'a' && c <= 'z'; }
constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }
constexpr bool is_letter(char c) noexcept { return is_upper(c) || is_lower(c); }
constexpr bool is_printable(char c) noexcept { return c >= 0x20 && c <= 0x7e; }
constexpr bool one_of(char c, std::string_view set) noexcept { return set.find(c) != std::string_view::npos; }

template <std::size_t N>
constexpr bool printable(const std::array<char, N>& a) noexcept {
  return std::all_of(a.begin(), a.end(), [](char c) { return is_printable(c); });
}

// Enumerations (spec §2.1, §2.2, §3.3, §3.4).
constexpr std::string_view kSides = "BSTE";
constexpr std::string_view kInboundDisplay = "AYNPIMWLOTQmnB";
// Accepted and Replaced list slightly different subsets of the inbound
// values plus 'Z' (changed to non-displayed); the union is accepted for both.
constexpr std::string_view kOutboundDisplay = "AYNPIMWLOTQmnBZ";
constexpr std::string_view kIso = "YNy";
constexpr std::string_view kCrossType = "NOCHSEA";
constexpr std::string_view kCustomerType = "RN ";
constexpr std::string_view kOrderState = "LD";
constexpr std::string_view kBboWeight = "0123SN ";
constexpr std::string_view kEventCode = "SE";

constexpr bool valid_side(Side s) noexcept { return one_of(static_cast<char>(s), kSides); }
constexpr bool valid_order_shares(std::uint32_t s) noexcept { return s > 0 && s < kSharesLimit; }

// --- Big-endian field access ----------------------------------------------
struct Reader {
  const std::uint8_t* p;
  [[nodiscard]] std::uint32_t u32(std::size_t off) const noexcept {
    return (static_cast<std::uint32_t>(p[off]) << 24U) | (static_cast<std::uint32_t>(p[off + 1]) << 16U) |
           (static_cast<std::uint32_t>(p[off + 2]) << 8U) | static_cast<std::uint32_t>(p[off + 3]);
  }
  [[nodiscard]] std::uint64_t u64(std::size_t off) const noexcept {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < 8; ++i) v = (v << 8U) | p[off + i];
    return v;
  }
  [[nodiscard]] char ch(std::size_t off) const noexcept { return static_cast<char>(p[off]); }
  template <std::size_t N>
  [[nodiscard]] std::array<char, N> alpha(std::size_t off) const noexcept {
    std::array<char, N> a{};
    for (std::size_t i = 0; i < N; ++i) a[i] = static_cast<char>(p[off + i]);
    return a;
  }
};

struct Writer {
  std::uint8_t* p;
  void u32(std::size_t off, std::uint32_t v) const noexcept {
    for (std::size_t i = 0; i < 4; ++i) p[off + i] = static_cast<std::uint8_t>(v >> (8U * (3 - i)));
  }
  void u64(std::size_t off, std::uint64_t v) const noexcept {
    for (std::size_t i = 0; i < 8; ++i) p[off + i] = static_cast<std::uint8_t>(v >> (8U * (7 - i)));
  }
  void ch(std::size_t off, char c) const noexcept { p[off] = static_cast<std::uint8_t>(c); }
  template <std::size_t N>
  void alpha(std::size_t off, const std::array<char, N>& a) const noexcept {
    for (std::size_t i = 0; i < N; ++i) p[off + i] = static_cast<std::uint8_t>(a[i]);
  }
};

// --- Parsers (length already checked) -------------------------------------
EnterOrder parse_enter(Reader r) noexcept {
  EnterOrder m;
  m.token = r.alpha<14>(1);
  m.side = static_cast<Side>(r.ch(15));
  m.shares = r.u32(16);
  m.stock = r.alpha<8>(20);
  m.price = r.u32(28);
  m.time_in_force = r.u32(32);
  m.firm = r.alpha<4>(36);
  m.display = r.ch(40);
  m.capacity = r.ch(41);
  m.iso = r.ch(42);
  m.min_qty = r.u32(43);
  m.cross_type = r.ch(47);
  m.customer_type = r.ch(48);
  return m;
}

ReplaceOrder parse_replace(Reader r) noexcept {
  ReplaceOrder m;
  m.existing = r.alpha<14>(1);
  m.replacement = r.alpha<14>(15);
  m.shares = r.u32(29);
  m.price = r.u32(33);
  m.time_in_force = r.u32(37);
  m.display = r.ch(41);
  m.iso = r.ch(42);
  m.min_qty = r.u32(43);
  return m;
}

CancelOrder parse_cancel(Reader r) noexcept { return CancelOrder{r.alpha<14>(1), r.u32(15)}; }

SystemEvent parse_system(Reader r) noexcept { return SystemEvent{r.u64(1), r.ch(9)}; }

Accepted parse_accepted(Reader r) noexcept {
  Accepted m;
  m.timestamp = r.u64(1);
  m.token = r.alpha<14>(9);
  m.side = static_cast<Side>(r.ch(23));
  m.shares = r.u32(24);
  m.stock = r.alpha<8>(28);
  m.price = r.u32(36);
  m.time_in_force = r.u32(40);
  m.firm = r.alpha<4>(44);
  m.display = r.ch(48);
  m.order_ref = r.u64(49);
  m.capacity = r.ch(57);
  m.iso = r.ch(58);
  m.min_qty = r.u32(59);
  m.cross_type = r.ch(63);
  m.order_state = r.ch(64);
  m.bbo_weight = r.ch(65);
  return m;
}

Replaced parse_replaced(Reader r) noexcept {
  Replaced m;
  m.timestamp = r.u64(1);
  m.replacement = r.alpha<14>(9);
  m.side = static_cast<Side>(r.ch(23));
  m.shares = r.u32(24);
  m.stock = r.alpha<8>(28);
  m.price = r.u32(36);
  m.time_in_force = r.u32(40);
  m.firm = r.alpha<4>(44);
  m.display = r.ch(48);
  m.order_ref = r.u64(49);
  m.capacity = r.ch(57);
  m.iso = r.ch(58);
  m.min_qty = r.u32(59);
  m.cross_type = r.ch(63);
  m.order_state = r.ch(64);
  m.previous = r.alpha<14>(65);
  m.bbo_weight = r.ch(79);
  return m;
}

Canceled parse_canceled(Reader r) noexcept { return Canceled{r.u64(1), r.alpha<14>(9), r.u32(23), r.ch(27)}; }

Executed parse_executed(Reader r) noexcept {
  return Executed{r.u64(1), r.alpha<14>(9), r.u32(23), r.u32(27), r.ch(31), r.u64(32)};
}

Rejected parse_rejected(Reader r) noexcept { return Rejected{r.u64(1), r.alpha<14>(9), r.ch(23)}; }

template <class M, class V, class Parse>
DecodeStatus finish(Reader r, Parse parse, V& out) noexcept {
  const M m = parse(r);
  if (!valid(m)) return DecodeStatus::InvalidField;
  out = m;
  return DecodeStatus::Decoded;
}

}  // namespace

std::string_view to_string(DecodeStatus s) noexcept {
  switch (s) {
    case DecodeStatus::Decoded: return "Decoded";
    case DecodeStatus::Skipped: return "Skipped";
    case DecodeStatus::Empty: return "Empty";
    case DecodeStatus::UnknownType: return "UnknownType";
    case DecodeStatus::BadLength: return "BadLength";
    case DecodeStatus::InvalidField: return "InvalidField";
  }
  return "?";
}

std::size_t inbound_length(char type) noexcept {
  switch (type) {
    case 'O': return kEnterOrderSize;
    case 'U': return kReplaceOrderSize;
    case 'X': return kCancelOrderSize;
    case 'M': return 20;  // Modify Order (not in the subset)
    default: return 0;
  }
}

std::size_t outbound_length(char type) noexcept {
  switch (type) {
    case 'S': return kSystemEventSize;
    case 'A': return kAcceptedSize;
    case 'U': return kReplacedSize;
    case 'C': return kCanceledSize;
    case 'E': return kExecutedSize;
    case 'J': return kRejectedSize;
    // Outside the subset (lengths from the field tables of the specification).
    case 'B': return 32;  // Broken Trade
    case 'G': return 45;  // Executed with Reference Price
    case 'D': return 38;  // AIQ Cancelled
    case 'P': return 23;  // Cancel Pending
    case 'I': return 23;  // Cancel Reject
    case 'T': return 36;  // Order Priority Update
    case 'M': return 28;  // Order Modified
    default: return 0;
  }
}

bool valid_token(const Token& t) noexcept {
  return std::all_of(t.begin(), t.end(), [](char c) { return is_letter(c) || is_digit(c) || c == ' '; });
}

bool is_market_price(std::uint32_t price) noexcept { return price == kMarketPrice || price == kMarketPriceMaxInt; }

bool valid_order_price(std::uint32_t price) noexcept { return price <= kMaxPrice || is_market_price(price); }

bool valid(const EnterOrder& m) noexcept {
  return valid_token(m.token) && valid_side(m.side) && valid_order_shares(m.shares) && printable(m.stock) &&
         valid_order_price(m.price) && printable(m.firm) && one_of(m.display, kInboundDisplay) &&
         is_printable(m.capacity) && one_of(m.iso, kIso) && one_of(m.cross_type, kCrossType) &&
         one_of(m.customer_type, kCustomerType);
}

bool valid(const ReplaceOrder& m) noexcept {
  return valid_token(m.existing) && valid_token(m.replacement) && valid_order_shares(m.shares) &&
         valid_order_price(m.price) && one_of(m.display, kInboundDisplay) && one_of(m.iso, kIso);
}

bool valid(const CancelOrder& m) noexcept { return valid_token(m.token); }

bool valid(const SystemEvent& m) noexcept { return m.timestamp < kNanosPerDay && one_of(m.event_code, kEventCode); }

bool valid(const Accepted& m) noexcept {
  return m.timestamp < kNanosPerDay && valid_token(m.token) && valid_side(m.side) && printable(m.stock) &&
         valid_order_price(m.price) && printable(m.firm) && one_of(m.display, kOutboundDisplay) &&
         is_printable(m.capacity) && one_of(m.iso, kIso) && one_of(m.cross_type, kCrossType) &&
         one_of(m.order_state, kOrderState) && one_of(m.bbo_weight, kBboWeight);
}

bool valid(const Replaced& m) noexcept {
  return m.timestamp < kNanosPerDay && valid_token(m.replacement) && valid_side(m.side) && printable(m.stock) &&
         valid_order_price(m.price) && printable(m.firm) && one_of(m.display, kOutboundDisplay) &&
         is_printable(m.capacity) && one_of(m.iso, kIso) && one_of(m.cross_type, kCrossType) &&
         one_of(m.order_state, kOrderState) && valid_token(m.previous) && one_of(m.bbo_weight, kBboWeight);
}

bool valid(const Canceled& m) noexcept { return m.timestamp < kNanosPerDay && valid_token(m.token) && is_upper(m.reason); }

bool valid(const Executed& m) noexcept {
  return m.timestamp < kNanosPerDay && valid_token(m.token) && m.price <= kMaxPrice &&
         (is_letter(m.liquidity) || is_digit(m.liquidity));
}

bool valid(const Rejected& m) noexcept { return m.timestamp < kNanosPerDay && valid_token(m.token) && is_letter(m.reason); }

DecodeStatus decode_inbound(Bytes payload, Inbound& out) noexcept {
  if (payload.empty()) return DecodeStatus::Empty;
  const char type = static_cast<char>(payload[0]);
  const std::size_t len = inbound_length(type);
  if (len == 0) return DecodeStatus::UnknownType;
  if (payload.size() != len) return DecodeStatus::BadLength;
  const Reader r{payload.data()};
  switch (type) {
    case 'O': return finish<EnterOrder>(r, parse_enter, out);
    case 'U': return finish<ReplaceOrder>(r, parse_replace, out);
    case 'X': return finish<CancelOrder>(r, parse_cancel, out);
    default: return DecodeStatus::Skipped;
  }
}

DecodeStatus decode_outbound(Bytes payload, Outbound& out) noexcept {
  if (payload.empty()) return DecodeStatus::Empty;
  const char type = static_cast<char>(payload[0]);
  const std::size_t len = outbound_length(type);
  if (len == 0) return DecodeStatus::UnknownType;
  if (payload.size() != len) return DecodeStatus::BadLength;
  const Reader r{payload.data()};
  switch (type) {
    case 'S': return finish<SystemEvent>(r, parse_system, out);
    case 'A': return finish<Accepted>(r, parse_accepted, out);
    case 'U': return finish<Replaced>(r, parse_replaced, out);
    case 'C': return finish<Canceled>(r, parse_canceled, out);
    case 'E': return finish<Executed>(r, parse_executed, out);
    case 'J': return finish<Rejected>(r, parse_rejected, out);
    default: return DecodeStatus::Skipped;
  }
}

// --- Encoders ---------------------------------------------------------------

std::size_t encode(const EnterOrder& m, std::span<std::uint8_t> out) noexcept {
  if (out.size() < kEnterOrderSize) return 0;
  const Writer w{out.data()};
  w.ch(0, 'O');
  w.alpha(1, m.token);
  w.ch(15, static_cast<char>(m.side));
  w.u32(16, m.shares);
  w.alpha(20, m.stock);
  w.u32(28, m.price);
  w.u32(32, m.time_in_force);
  w.alpha(36, m.firm);
  w.ch(40, m.display);
  w.ch(41, m.capacity);
  w.ch(42, m.iso);
  w.u32(43, m.min_qty);
  w.ch(47, m.cross_type);
  w.ch(48, m.customer_type);
  return kEnterOrderSize;
}

std::size_t encode(const ReplaceOrder& m, std::span<std::uint8_t> out) noexcept {
  if (out.size() < kReplaceOrderSize) return 0;
  const Writer w{out.data()};
  w.ch(0, 'U');
  w.alpha(1, m.existing);
  w.alpha(15, m.replacement);
  w.u32(29, m.shares);
  w.u32(33, m.price);
  w.u32(37, m.time_in_force);
  w.ch(41, m.display);
  w.ch(42, m.iso);
  w.u32(43, m.min_qty);
  return kReplaceOrderSize;
}

std::size_t encode(const CancelOrder& m, std::span<std::uint8_t> out) noexcept {
  if (out.size() < kCancelOrderSize) return 0;
  const Writer w{out.data()};
  w.ch(0, 'X');
  w.alpha(1, m.token);
  w.u32(15, m.shares);
  return kCancelOrderSize;
}

std::size_t encode(const SystemEvent& m, std::span<std::uint8_t> out) noexcept {
  if (out.size() < kSystemEventSize) return 0;
  const Writer w{out.data()};
  w.ch(0, 'S');
  w.u64(1, m.timestamp);
  w.ch(9, m.event_code);
  return kSystemEventSize;
}

std::size_t encode(const Accepted& m, std::span<std::uint8_t> out) noexcept {
  if (out.size() < kAcceptedSize) return 0;
  const Writer w{out.data()};
  w.ch(0, 'A');
  w.u64(1, m.timestamp);
  w.alpha(9, m.token);
  w.ch(23, static_cast<char>(m.side));
  w.u32(24, m.shares);
  w.alpha(28, m.stock);
  w.u32(36, m.price);
  w.u32(40, m.time_in_force);
  w.alpha(44, m.firm);
  w.ch(48, m.display);
  w.u64(49, m.order_ref);
  w.ch(57, m.capacity);
  w.ch(58, m.iso);
  w.u32(59, m.min_qty);
  w.ch(63, m.cross_type);
  w.ch(64, m.order_state);
  w.ch(65, m.bbo_weight);
  return kAcceptedSize;
}

std::size_t encode(const Replaced& m, std::span<std::uint8_t> out) noexcept {
  if (out.size() < kReplacedSize) return 0;
  const Writer w{out.data()};
  w.ch(0, 'U');
  w.u64(1, m.timestamp);
  w.alpha(9, m.replacement);
  w.ch(23, static_cast<char>(m.side));
  w.u32(24, m.shares);
  w.alpha(28, m.stock);
  w.u32(36, m.price);
  w.u32(40, m.time_in_force);
  w.alpha(44, m.firm);
  w.ch(48, m.display);
  w.u64(49, m.order_ref);
  w.ch(57, m.capacity);
  w.ch(58, m.iso);
  w.u32(59, m.min_qty);
  w.ch(63, m.cross_type);
  w.ch(64, m.order_state);
  w.alpha(65, m.previous);
  w.ch(79, m.bbo_weight);
  return kReplacedSize;
}

std::size_t encode(const Canceled& m, std::span<std::uint8_t> out) noexcept {
  if (out.size() < kCanceledSize) return 0;
  const Writer w{out.data()};
  w.ch(0, 'C');
  w.u64(1, m.timestamp);
  w.alpha(9, m.token);
  w.u32(23, m.decrement);
  w.ch(27, m.reason);
  return kCanceledSize;
}

std::size_t encode(const Executed& m, std::span<std::uint8_t> out) noexcept {
  if (out.size() < kExecutedSize) return 0;
  const Writer w{out.data()};
  w.ch(0, 'E');
  w.u64(1, m.timestamp);
  w.alpha(9, m.token);
  w.u32(23, m.shares);
  w.u32(27, m.price);
  w.ch(31, m.liquidity);
  w.u64(32, m.match_number);
  return kExecutedSize;
}

std::size_t encode(const Rejected& m, std::span<std::uint8_t> out) noexcept {
  if (out.size() < kRejectedSize) return 0;
  const Writer w{out.data()};
  w.ch(0, 'J');
  w.u64(1, m.timestamp);
  w.alpha(9, m.token);
  w.ch(23, m.reason);
  return kRejectedSize;
}

std::size_t encode(const Inbound& m, std::span<std::uint8_t> out) noexcept {
  return std::visit([&](const auto& x) { return encode(x, out); }, m);
}

std::size_t encode(const Outbound& m, std::span<std::uint8_t> out) noexcept {
  return std::visit([&](const auto& x) { return encode(x, out); }, m);
}

}  // namespace lob::ouch
