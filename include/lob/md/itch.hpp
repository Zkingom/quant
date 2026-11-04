#pragma once

// Nasdaq TotalView-ITCH 5.0 subset decoder (L3' 1b).
//
// Source of truth: "Nasdaq TotalView-ITCH 5.0" specification (Nasdaq,
// 2020 revision). All integers are big-endian, unsigned; Price(4) has 4
// implied decimals; Timestamp is 6 bytes of nanoseconds since midnight.
// Every message starts with the common 11-byte header:
//   offset 0  Message Type     1  alpha
//   offset 1  Stock Locate     2  integer
//   offset 3  Tracking Number  2  integer
//   offset 5  Timestamp        6  integer
//
// Decoded subset (message lengths, header included):
//   S System Event 12 · R Stock Directory 39 · A Add Order 36 ·
//   F Add Order with MPID 40 · E Order Executed 31 ·
//   C Order Executed with Price 36 · X Order Cancel 23 · D Order Delete 19 ·
//   U Order Replace 35 · H Stock Trading Action 25
// Every other message type defined by the 5.0 specification is recognised
// by its exact length and reported as Skipped (real files contain them);
// any other type byte is rejected.
//
// "Zero-copy": the decoder reads each field straight out of the caller's
// buffer (big-endian byte assembly, no type punning, no alignment
// assumptions) and never buffers or copies the frame. The result is a small
// trivially copyable value.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <variant>

namespace lob::md::itch {

using Bytes = std::span<const std::uint8_t>;
using Symbol = std::array<char, 8>;  ///< alpha, left-justified, space-padded

inline constexpr std::size_t kHeaderSize = 11;
/// 24 h in ns. Timestamps at or above this are rejected as InvalidField.
inline constexpr std::uint64_t kNanosPerDay = 86'400'000'000'000ULL;

struct Header {
  char type{};
  std::uint16_t stock_locate{};
  std::uint16_t tracking_number{};
  std::uint64_t timestamp{};  ///< ns since midnight, < kNanosPerDay
};

/// Event codes: O start of messages, S start of system hours, Q start of
/// market hours, M end of market hours, E end of system hours, C end of messages.
struct SystemEvent {
  Header header;
  char event_code{};
};

struct StockDirectory {
  Header header;
  Symbol stock{};
  char market_category{};
  char financial_status{};
  std::uint32_t round_lot_size{};
  char round_lots_only{};
  char issue_classification{};
  std::array<char, 2> issue_subtype{};
  char authenticity{};
  char short_sale_threshold{};
  char ipo_flag{};
  char luld_reference_price_tier{};
  char etp_flag{};
  std::uint32_t etp_leverage_factor{};
  char inverse_indicator{};
};

enum class Side : char { Buy = 'B', Sell = 'S' };

/// 'A' (has_mpid = false) and 'F' (has_mpid = true, attribution filled).
struct AddOrder {
  Header header;
  std::uint64_t order_ref{};
  Side side{};
  std::uint32_t shares{};
  Symbol stock{};
  std::uint32_t price{};
  std::array<char, 4> attribution{};
  bool has_mpid{};
};

struct OrderExecuted {
  Header header;
  std::uint64_t order_ref{};
  std::uint32_t executed_shares{};
  std::uint64_t match_number{};
};

struct OrderExecutedWithPrice {
  Header header;
  std::uint64_t order_ref{};
  std::uint32_t executed_shares{};
  std::uint64_t match_number{};
  char printable{};  ///< 'Y' or 'N'
  std::uint32_t execution_price{};
};

struct OrderCancel {
  Header header;
  std::uint64_t order_ref{};
  std::uint32_t cancelled_shares{};
};

struct OrderDelete {
  Header header;
  std::uint64_t order_ref{};
};

struct OrderReplace {
  Header header;
  std::uint64_t original_order_ref{};
  std::uint64_t new_order_ref{};
  std::uint32_t shares{};
  std::uint32_t price{};
};

/// 'H' Stock Trading Action. Trading State: 'H' halted, 'P' paused (LULD
/// pause), 'Q' quotation only (halt/IPO quoting period, no trading),
/// 'T' trading. Reason is a 4-character code (e.g. "T1  ").
struct TradingAction {
  Header header;
  Symbol stock{};
  char trading_state{};
  char reserved{};
  std::array<char, 4> reason{};
};

using Message = std::variant<SystemEvent, StockDirectory, AddOrder, OrderExecuted,
                             OrderExecutedWithPrice, OrderCancel, OrderDelete, OrderReplace,
                             TradingAction>;
static_assert(std::is_trivially_copyable_v<Message>, "decoded messages cross the SPSC queue");

[[nodiscard]] inline const Header& header_of(const Message& m) noexcept {
  return std::visit([](const auto& x) -> const Header& { return x.header; }, m);
}

enum class DecodeStatus : std::uint8_t {
  Decoded,       ///< `out` holds the message
  Skipped,       ///< a valid 5.0 message type outside the subset (length checked)
  Empty,         ///< zero-length payload
  UnknownType,   ///< type byte not defined by ITCH 5.0
  BadLength,     ///< payload length differs from the specification for its type
  InvalidField,  ///< enumerated field out of range, or timestamp >= 24 h
};

[[nodiscard]] std::string_view to_string(DecodeStatus s) noexcept;

/// Exact specification length of message type `type` (header included), or
/// 0 if `type` is not an ITCH 5.0 message type.
[[nodiscard]] std::size_t spec_length(char type) noexcept;

/// Decodes one message payload (the bytes after the 2-byte length prefix).
/// Never throws, never reads outside `payload`. On any status other than
/// Decoded, `out` is left unchanged.
[[nodiscard]] DecodeStatus decode(Bytes payload, Message& out) noexcept;

// ---------------------------------------------------------------------------
// Framing of the Nasdaq sample files: each message is preceded by a 2-byte
// big-endian length. A FrameCursor walks a contiguous buffer; when the buffer
// is one chunk of a larger stream, Truncated at the end means "need more
// bytes" and the caller carries the unconsumed tail into the next chunk.

enum class FrameStatus : std::uint8_t {
  Frame,       ///< `payload` set, cursor advanced
  End,         ///< cursor is exactly at the end of the buffer
  Truncated,   ///< fewer bytes remain than the next frame needs (cursor unchanged)
  ZeroLength,  ///< length prefix 0: invalid in ITCH (cursor unchanged)
};

class FrameCursor {
 public:
  explicit FrameCursor(Bytes buffer) noexcept : buffer_(buffer) {}

  [[nodiscard]] FrameStatus next(Bytes& payload) noexcept;

  /// Bytes consumed so far (offset of the next length prefix).
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

 private:
  Bytes buffer_;
  std::size_t offset_{0};
};

// Big-endian field readers (memcpy-free byte assembly; usable in constexpr
// test code). Preconditions: p points to at least N readable bytes.
[[nodiscard]] constexpr std::uint16_t be16(const std::uint8_t* p) noexcept {
  return static_cast<std::uint16_t>((static_cast<unsigned>(p[0]) << 8U) | p[1]);
}
[[nodiscard]] constexpr std::uint32_t be32(const std::uint8_t* p) noexcept {
  return (static_cast<std::uint32_t>(p[0]) << 24U) | (static_cast<std::uint32_t>(p[1]) << 16U) |
         (static_cast<std::uint32_t>(p[2]) << 8U) | static_cast<std::uint32_t>(p[3]);
}
[[nodiscard]] constexpr std::uint64_t be48(const std::uint8_t* p) noexcept {
  std::uint64_t v = 0;
  for (int i = 0; i < 6; ++i) {
    v = (v << 8U) | p[i];
  }
  return v;
}
[[nodiscard]] constexpr std::uint64_t be64(const std::uint8_t* p) noexcept {
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v = (v << 8U) | p[i];
  }
  return v;
}

}  // namespace lob::md::itch
