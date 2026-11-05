#pragma once

// SoupBinTCP 3.00 framing and packet helpers (L4.2). docs/design/L4_DESIGN.md §2.
//
// A logical packet is a 2-byte big-endian length (counting the 1-byte type
// and the payload, not itself), the type, and the payload. Logical packets
// have no relation to TCP segments: they arrive split and coalesced.
//
//   server -> client  '+' Debug · 'A' Login Accepted (session 10, sequence 20)
//                     'J' Login Rejected (reason 1) · 'S' Sequenced Data
//                     'H' Server Heartbeat · 'Z' End of Session
//   client -> server  '+' Debug · 'L' Login Request (user 6, password 10,
//                     session 10, sequence 20) · 'U' Unsequenced Data
//                     'R' Client Heartbeat · 'O' Logout Request
//
// Limits [project convention; the specification defines no maximum]: a
// data / control packet may be at most kMaxPacket bytes long (type
// included), a Debug packet at most kMaxDebugPacket. Longer, or length 0
// (the type byte is mandatory), is a protocol error.
//
// Everything here is sans-IO: no sockets, no clocks. Bytes go in, bytes to
// send accumulate in a fixed-capacity OutBuffer, time is a parameter.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace lob::soup {

using Bytes = std::span<const std::uint8_t>;
using SessionId = std::array<char, 10>;

inline constexpr std::size_t kMaxPacket = 256;
inline constexpr std::size_t kMaxDebugPacket = 4096;
inline constexpr std::size_t kLoginRequestLength = 1 + 6 + 10 + 10 + 20;  // 47
inline constexpr std::size_t kLoginAcceptedLength = 1 + 10 + 20;          // 31

namespace type {
inline constexpr char kDebug = '+';
inline constexpr char kLoginAccepted = 'A';
inline constexpr char kLoginRejected = 'J';
inline constexpr char kSequencedData = 'S';
inline constexpr char kServerHeartbeat = 'H';
inline constexpr char kEndOfSession = 'Z';
inline constexpr char kLoginRequest = 'L';
inline constexpr char kUnsequencedData = 'U';
inline constexpr char kClientHeartbeat = 'R';
inline constexpr char kLogoutRequest = 'O';
}  // namespace type

/// Login Rejected reason codes.
inline constexpr char kRejectNotAuthorized = 'A';
inline constexpr char kRejectSessionNotAvailable = 'S';

/// Timers (SoupBinTCP §1.3, §2.3.1).
struct Timers {
  std::uint64_t heartbeat_ns{1'000'000'000ULL};  ///< send a heartbeat after this long without sending
  std::uint64_t timeout_ns{15'000'000'000ULL};   ///< link lost after this long without receiving
  std::uint64_t login_timeout_ns{30'000'000'000ULL};  ///< server: no Login Request this long after accept
};

/// Bytes waiting to be written to the socket. Fixed capacity, reserved at
/// construction; append() refuses (returns false) rather than grow.
class OutBuffer {
 public:
  explicit OutBuffer(std::size_t capacity) : buf_(capacity) {}

  [[nodiscard]] bool append_packet(char t, Bytes payload) noexcept;
  [[nodiscard]] std::size_t free_space() const noexcept { return buf_.size() - (end_ - begin_); }
  [[nodiscard]] bool empty() const noexcept { return begin_ == end_; }
  [[nodiscard]] Bytes pending() const noexcept { return {buf_.data() + begin_, end_ - begin_}; }
  void consume(std::size_t n) noexcept;
  void clear() noexcept { begin_ = end_ = 0; }
  [[nodiscard]] std::size_t capacity() const noexcept { return buf_.size(); }

 private:
  std::vector<std::uint8_t> buf_;
  std::size_t begin_{0};
  std::size_t end_{0};
};

enum class FrameError : std::uint8_t { None, ZeroLength, TooLong };

[[nodiscard]] std::string_view to_string(FrameError e) noexcept;

/// Splits a byte stream into logical packets. Complete packets inside one
/// input chunk are delivered in place (zero-copy); only a packet that spans
/// chunks is assembled in a fixed internal buffer.
class Deframer {
 public:
  /// Calls on_packet(char type, Bytes payload) for every complete packet;
  /// on_packet returns false to stop (the rest of the input is dropped).
  /// Returns the framing error that stopped it, if any; after an error the
  /// deframer stays failed.
  template <class F>
  FrameError feed(Bytes in, F&& on_packet);

  [[nodiscard]] FrameError error() const noexcept { return error_; }
  void reset() noexcept {
    have_ = 0;
    error_ = FrameError::None;
  }

 private:
  /// Length check of a packet whose 2-byte length prefix is `len` and
  /// whose first byte (type) is `t`, or -1 when the type is not known yet.
  [[nodiscard]] FrameError check(std::size_t len, int t) const noexcept;

  std::array<std::uint8_t, 2 + kMaxDebugPacket> part_{};
  std::size_t have_{0};
  FrameError error_{FrameError::None};
};

template <class F>
FrameError Deframer::feed(Bytes in, F&& on_packet) {
  if (error_ != FrameError::None) return error_;
  std::size_t i = 0;
  // 1. finish a packet that started in an earlier chunk
  while (have_ > 0 && i < in.size()) {
    part_[have_++] = in[i++];
    if (have_ < 2) continue;
    const std::size_t len = (static_cast<std::size_t>(part_[0]) << 8U) | part_[1];
    if (const FrameError e = check(len, have_ >= 3 ? part_[2] : -1); e != FrameError::None) return error_ = e;
    if (have_ == 2 + len) {
      have_ = 0;
      if (!on_packet(static_cast<char>(part_[2]), Bytes{part_.data() + 3, len - 1})) return error_;
    }
  }
  // 2. whole packets in place
  while (i + 2 <= in.size()) {
    const std::size_t len = (static_cast<std::size_t>(in[i]) << 8U) | in[i + 1];
    if (const FrameError e = check(len, i + 2 < in.size() ? in[i + 2] : -1); e != FrameError::None) {
      return error_ = e;
    }
    if (i + 2 + len > in.size()) break;
    const char t = static_cast<char>(in[i + 2]);
    const Bytes payload{in.data() + i + 3, len - 1};
    i += 2 + len;
    if (!on_packet(t, payload)) return error_;
  }
  // 3. keep the tail (a partial packet) for the next chunk
  while (i < in.size()) part_[have_++] = in[i++];
  return error_;
}

// --- Packet builders (append to an OutBuffer) ------------------------------
[[nodiscard]] bool put_login_request(OutBuffer& out, std::string_view user, std::string_view password,
                                     const SessionId& session, std::uint64_t sequence) noexcept;
[[nodiscard]] bool put_login_accepted(OutBuffer& out, const SessionId& session, std::uint64_t sequence) noexcept;

/// Fields of a Login Request / Login Accepted payload (after the type byte).
struct LoginRequest {
  std::array<char, 6> user{};
  std::array<char, 10> password{};
  SessionId session{};
  std::uint64_t sequence{0};
};
struct LoginAccepted {
  SessionId session{};
  std::uint64_t sequence{0};
};
/// False when the payload length or the numeric field is malformed.
[[nodiscard]] bool parse_login_request(Bytes payload, LoginRequest& out) noexcept;
[[nodiscard]] bool parse_login_accepted(Bytes payload, LoginAccepted& out) noexcept;

/// Space-padded alpha field; `left_pad` right-aligns (Login Accepted session).
template <std::size_t N>
[[nodiscard]] constexpr std::array<char, N> field(std::string_view s, bool left_pad = false) noexcept {
  std::array<char, N> a{};
  a.fill(' ');
  const std::size_t n = s.size() < N ? s.size() : N;
  const std::size_t off = left_pad ? N - n : 0;
  for (std::size_t i = 0; i < n; ++i) a[off + i] = s[i];
  return a;
}
[[nodiscard]] constexpr bool blank(const SessionId& s) noexcept {
  for (const char c : s) {
    if (c != ' ') return false;
  }
  return true;
}
/// Case-insensitive comparison ignoring leading / trailing spaces
/// (username / password are case-insensitive; session ids are compared
/// after trimming because Login Accepted left-pads and requests right-pad).
[[nodiscard]] bool same_field(std::string_view a, std::string_view b, bool case_insensitive) noexcept;

}  // namespace lob::soup
