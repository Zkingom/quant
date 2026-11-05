#pragma once

// Nasdaq OUCH 4.2 subset codec (L4.1). docs/design/L4_DESIGN.md §1.
//
// Source of truth: "O*U*C*H Version 4.2" (Nasdaq, updated October 2025).
// Integers are unsigned big-endian; alpha fields left-justified and
// space-padded; prices have 4 implied decimals (same unit as ITCH 5.0);
// outbound timestamps are 8 bytes of nanoseconds since midnight. Every
// message type has a fixed length, and the codec accepts a payload only at
// exactly that length.
//
//   inbound  (client -> exchange, unsequenced)
//     O Enter Order 49 · U Replace Order 47 · X Cancel Order 19
//     M Modify Order 20 is a spec type outside the subset: Skipped
//   outbound (exchange -> client, sequenced by the session layer)
//     S System Event 10 · A Accepted 66 · U Replaced 80 · C Canceled 28 ·
//     E Executed 40 · J Rejected 24
//     B Broken Trade 32 · G Executed with Reference Price 45 ·
//     D AIQ Cancelled 38 · P Cancel Pending 23 · I Cancel Reject 23 ·
//     T Order Priority Update 36 · M Order Modified 28: Skipped
// Type bytes are direction-specific ('U' is Replace Order inbound and
// Replaced outbound; 'M' is Modify / Order Modified), hence two decoders.
//
// Validation is what the SPECIFICATION defines -- the codec does not apply
// the exchange's subset policy (market orders, display types, TIF values
// the simulated exchange refuses are a server decision, L4.3):
//   - token: letters, digits, spaces (§1.2)
//   - side: B S T E; order state: L D; system event: S E
//   - Enter / Replace shares: 0 < shares < 1,000,000
//   - price (Enter / Replace / Accepted / Replaced): <= $199,999.9900, or
//     one of the two market-order values $200,000.0000 and 0x7FFFFFFF;
//     execution price: <= $199,999.9900
//   - display, ISO eligibility, cross type, customer type, BBO weight:
//     the values listed by the specification
//   - Canceled reason: a capital letter ("support all capital letters");
//     Rejected reason: a letter (the table uses both cases); liquidity
//     flag: a letter or a digit
//   - other alpha fields (stock, firm, capacity): printable ASCII
//   - timestamp < 24 h
// Fields with no constraint in the specification (Cancel shares, TIF,
// minimum quantity, reference and match numbers, decrement and executed
// shares) are accepted as they are. Every Decoded payload re-encodes to
// exactly the same bytes (all fields are kept raw; tested and fuzzed).

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <variant>

namespace lob::ouch {

using Bytes = std::span<const std::uint8_t>;
using Token = std::array<char, 14>;
using Symbol = std::array<char, 8>;
using Firm = std::array<char, 4>;

/// $199,999.9900 (0x7735939C): the largest limit price.
inline constexpr std::uint32_t kMaxPrice = 1'999'999'900U;
/// The two prices the specification treats as market orders.
inline constexpr std::uint32_t kMarketPrice = 2'000'000'000U;
inline constexpr std::uint32_t kMarketPriceMaxInt = 0x7FFFFFFFU;
/// Enter / Replace shares must be below this.
inline constexpr std::uint32_t kSharesLimit = 1'000'000U;
inline constexpr std::uint64_t kNanosPerDay = 86'400'000'000'000ULL;

/// Time in Force special values (§1.2); any other value is a number of seconds.
inline constexpr std::uint32_t kTifImmediateOrCancel = 0;
inline constexpr std::uint32_t kTifExtendedTradingClose = 99'996;
inline constexpr std::uint32_t kTifMarketHours = 99'998;
inline constexpr std::uint32_t kTifSystemHours = 99'999;

enum class Side : char { Buy = 'B', Sell = 'S', SellShort = 'T', SellShortExempt = 'E' };

// --- Exact lengths --------------------------------------------------------
inline constexpr std::size_t kEnterOrderSize = 49;
inline constexpr std::size_t kReplaceOrderSize = 47;
inline constexpr std::size_t kCancelOrderSize = 19;
inline constexpr std::size_t kSystemEventSize = 10;
inline constexpr std::size_t kAcceptedSize = 66;
inline constexpr std::size_t kReplacedSize = 80;
inline constexpr std::size_t kCanceledSize = 28;
inline constexpr std::size_t kExecutedSize = 40;
inline constexpr std::size_t kRejectedSize = 24;
/// Longest message of the subset (encode buffers of this size always suffice).
inline constexpr std::size_t kMaxMessageSize = kReplacedSize;

// --- Inbound --------------------------------------------------------------

/// 'O' Enter Order.
struct EnterOrder {
  Token token{};
  Side side{Side::Buy};
  std::uint32_t shares{0};
  Symbol stock{};
  std::uint32_t price{0};
  std::uint32_t time_in_force{0};
  Firm firm{};
  char display{'A'};
  char capacity{'P'};
  char iso{'N'};  ///< Intermarket Sweep Eligibility
  std::uint32_t min_qty{0};
  char cross_type{'N'};
  char customer_type{' '};
};

/// 'U' Replace Order. `shares` is the total liable for the whole
/// order/replace chain, previous executions included (§2.2).
struct ReplaceOrder {
  Token existing{};
  Token replacement{};
  std::uint32_t shares{0};
  std::uint32_t price{0};
  std::uint32_t time_in_force{0};
  char display{'A'};
  char iso{'N'};
  std::uint32_t min_qty{0};
};

/// 'X' Cancel Order. `shares` is the new INTENDED ORDER SIZE (0 = cancel
/// everything), not a decrement -- so a repeated cancel is harmless (§2.3).
struct CancelOrder {
  Token token{};
  std::uint32_t shares{0};
};

using Inbound = std::variant<EnterOrder, ReplaceOrder, CancelOrder>;

// --- Outbound -------------------------------------------------------------

/// 'S' System Event: 'S' start of day, 'E' end of day.
struct SystemEvent {
  std::uint64_t timestamp{0};
  char event_code{'S'};
};

/// 'A' Accepted.
struct Accepted {
  std::uint64_t timestamp{0};
  Token token{};
  Side side{Side::Buy};
  std::uint32_t shares{0};
  Symbol stock{};
  std::uint32_t price{0};
  std::uint32_t time_in_force{0};
  Firm firm{};
  char display{'A'};
  std::uint64_t order_ref{0};  ///< Order Reference Number, day-unique
  char capacity{'P'};
  char iso{'N'};
  std::uint32_t min_qty{0};
  char cross_type{'N'};
  char order_state{'L'};  ///< 'L' live, 'D' dead (accepted and cancelled at once)
  char bbo_weight{' '};
};

/// 'U' Replaced. `shares` = shares left on the book after the replace.
struct Replaced {
  std::uint64_t timestamp{0};
  Token replacement{};
  Side side{Side::Buy};
  std::uint32_t shares{0};
  Symbol stock{};
  std::uint32_t price{0};
  std::uint32_t time_in_force{0};
  Firm firm{};
  char display{'A'};
  std::uint64_t order_ref{0};
  char capacity{'P'};
  char iso{'N'};
  std::uint32_t min_qty{0};
  char cross_type{'N'};
  char order_state{'L'};
  Token previous{};
  char bbo_weight{' '};
};

/// 'C' Canceled. `decrement` is incremental, not cumulative.
struct Canceled {
  std::uint64_t timestamp{0};
  Token token{};
  std::uint32_t decrement{0};
  char reason{'U'};
};

/// 'E' Executed. `shares` is incremental.
struct Executed {
  std::uint64_t timestamp{0};
  Token token{};
  std::uint32_t shares{0};
  std::uint32_t price{0};
  char liquidity{'A'};
  std::uint64_t match_number{0};
};

/// 'J' Rejected. The token cannot be reused.
struct Rejected {
  std::uint64_t timestamp{0};
  Token token{};
  char reason{'O'};
};

using Outbound = std::variant<SystemEvent, Accepted, Replaced, Canceled, Executed, Rejected>;

static_assert(std::is_trivially_copyable_v<Inbound>);
static_assert(std::is_trivially_copyable_v<Outbound>);

// Cancel / reject reason codes used by the simulated exchange (§3.5.1, §3.10.1).
namespace reason {
inline constexpr char kUserCancel = 'U';
inline constexpr char kImmediateOrCancel = 'I';
inline constexpr char kSystemCancel = 'Z';  ///< used for cancel-on-disconnect (L4 design §2.4)
inline constexpr char kRejectInvalidPrice = 'X';
inline constexpr char kRejectInvalidStock = 'S';
inline constexpr char kRejectHalted = 'H';
inline constexpr char kRejectClosed = 'C';
inline constexpr char kRejectOther = 'O';
inline constexpr char kRejectPortRate = 'x';  ///< Risk: Port Message Rate Restriction
}  // namespace reason

// --- Decoding -------------------------------------------------------------

enum class DecodeStatus : std::uint8_t {
  Decoded,       ///< `out` holds the message
  Skipped,       ///< a valid OUCH 4.2 type of this direction outside the subset (length checked)
  Empty,         ///< zero-length payload
  UnknownType,   ///< type byte not defined for this direction
  BadLength,     ///< payload length differs from the specification for its type
  InvalidField,  ///< a field violates the specification (see the header comment)
};

[[nodiscard]] std::string_view to_string(DecodeStatus s) noexcept;

/// Specification length of an inbound / outbound type, subset or not; 0 if
/// the type byte is not defined for that direction.
[[nodiscard]] std::size_t inbound_length(char type) noexcept;
[[nodiscard]] std::size_t outbound_length(char type) noexcept;

/// Never throws, never reads outside `payload`. On any status other than
/// Decoded, `out` is left unchanged.
[[nodiscard]] DecodeStatus decode_inbound(Bytes payload, Inbound& out) noexcept;
[[nodiscard]] DecodeStatus decode_outbound(Bytes payload, Outbound& out) noexcept;

// --- Field checks (the decoder's rules, exposed for the server / gateway) --
[[nodiscard]] bool valid_token(const Token& t) noexcept;
[[nodiscard]] bool valid_order_price(std::uint32_t price) noexcept;
[[nodiscard]] bool is_market_price(std::uint32_t price) noexcept;
[[nodiscard]] bool valid(const EnterOrder& m) noexcept;
[[nodiscard]] bool valid(const ReplaceOrder& m) noexcept;
[[nodiscard]] bool valid(const CancelOrder& m) noexcept;
[[nodiscard]] bool valid(const SystemEvent& m) noexcept;
[[nodiscard]] bool valid(const Accepted& m) noexcept;
[[nodiscard]] bool valid(const Replaced& m) noexcept;
[[nodiscard]] bool valid(const Canceled& m) noexcept;
[[nodiscard]] bool valid(const Executed& m) noexcept;
[[nodiscard]] bool valid(const Rejected& m) noexcept;

// --- Encoding -------------------------------------------------------------
// Writes the message into `out` and returns the number of bytes written
// (the message's fixed length), or 0 -- writing nothing -- if `out` is too
// small. Does not validate: encoding an invalid message is the caller's bug,
// and the tests check encode(decode(x)) == x for every valid x.
std::size_t encode(const EnterOrder& m, std::span<std::uint8_t> out) noexcept;
std::size_t encode(const ReplaceOrder& m, std::span<std::uint8_t> out) noexcept;
std::size_t encode(const CancelOrder& m, std::span<std::uint8_t> out) noexcept;
std::size_t encode(const SystemEvent& m, std::span<std::uint8_t> out) noexcept;
std::size_t encode(const Accepted& m, std::span<std::uint8_t> out) noexcept;
std::size_t encode(const Replaced& m, std::span<std::uint8_t> out) noexcept;
std::size_t encode(const Canceled& m, std::span<std::uint8_t> out) noexcept;
std::size_t encode(const Executed& m, std::span<std::uint8_t> out) noexcept;
std::size_t encode(const Rejected& m, std::span<std::uint8_t> out) noexcept;
std::size_t encode(const Inbound& m, std::span<std::uint8_t> out) noexcept;
std::size_t encode(const Outbound& m, std::span<std::uint8_t> out) noexcept;

// --- Helpers --------------------------------------------------------------
/// Left-justified, space-padded alpha field from a string (truncated to N).
template <std::size_t N>
[[nodiscard]] constexpr std::array<char, N> alpha(std::string_view s) noexcept {
  std::array<char, N> a{};
  for (std::size_t i = 0; i < N; ++i) a[i] = i < s.size() ? s[i] : ' ';
  return a;
}
[[nodiscard]] constexpr Token make_token(std::string_view s) noexcept { return alpha<14>(s); }
[[nodiscard]] constexpr Symbol make_symbol(std::string_view s) noexcept { return alpha<8>(s); }

}  // namespace lob::ouch
