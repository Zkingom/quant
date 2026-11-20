#pragma once

// MoldUDP64 downstream packet framing (L3' 1d) and, since L5.1, the encoder
// side: packet builder / packetizer, heartbeat, end of session, request
// packets. The decoder was first used with hand-built packets
// to exercise real sequence-number gaps; the Nasdaq sample files are not
// MoldUDP64 (they carry only length prefixes).
//
// Packet (Nasdaq MoldUDP64 Protocol Specification v1.00):
//   Session          10  alpha
//   Sequence Number   8  integer, seq of the first message in the packet
//   Message Count     2  integer; 0 = heartbeat, 0xFFFF = end of session
//   then Message Count blocks of: Message Length 2 + Message Data

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "lob/md/itch.hpp"

namespace lob::md::mold {

inline constexpr std::size_t kHeaderSize = 20;
inline constexpr std::uint16_t kEndOfSession = 0xFFFF;

struct Header {
  std::array<char, 10> session{};
  std::uint64_t sequence{0};
  std::uint16_t count{0};
};

enum class Status : std::uint8_t {
  Ok,            ///< `count` messages delivered
  Heartbeat,     ///< count 0
  EndOfSession,  ///< count 0xFFFF
  Truncated,     ///< shorter than the header or than its message blocks
  ZeroLength,    ///< a message block of length 0
  TrailingBytes, ///< bytes after the last message block
};

/// Parses one packet. Calls on_message(seq, payload) for each message block
/// before validating the remainder, so a damaged packet may deliver a valid
/// prefix; the status tells the caller what happened to the rest.
template <typename F>
Status parse_packet(itch::Bytes packet, Header& h, F&& on_message) {
  if (packet.size() < kHeaderSize) {
    return Status::Truncated;
  }
  for (std::size_t i = 0; i < 10; ++i) {
    h.session[i] = static_cast<char>(packet[i]);
  }
  h.sequence = itch::be64(packet.data() + 10);
  h.count = itch::be16(packet.data() + 18);
  if (h.count == 0) {
    return packet.size() == kHeaderSize ? Status::Heartbeat : Status::TrailingBytes;
  }
  if (h.count == kEndOfSession) {
    return packet.size() == kHeaderSize ? Status::EndOfSession : Status::TrailingBytes;
  }
  std::size_t off = kHeaderSize;
  for (std::uint16_t i = 0; i < h.count; ++i) {
    if (packet.size() - off < 2) {
      return Status::Truncated;
    }
    const std::size_t len = itch::be16(packet.data() + off);
    if (len == 0) {
      return Status::ZeroLength;
    }
    if (packet.size() - off - 2 < len) {
      return Status::Truncated;
    }
    on_message(h.sequence + i, packet.subspan(off + 2, len));
    off += 2 + len;
  }
  return off == packet.size() ? Status::Ok : Status::TrailingBytes;
}

// ---------------------------------------------------------------------------
// Encoding (L5.1). Everything writes into caller-provided buffers and never
// allocates, so the sender and the retransmission service can run on
// preallocated storage.

using Session = std::array<char, 10>;

/// Request Packet (unicast to the retransmission server): Session 10,
/// Sequence Number 8 (first requested message), Requested Message Count 2.
inline constexpr std::size_t kRequestSize = 20;
/// Largest count a message-carrying packet may declare; 0xFFFF means end of session.
inline constexpr std::uint16_t kMaxMessageCount = 0xFFFE;
/// [project convention] Largest downstream packet (the whole UDP payload,
/// header included). 1 400 bytes stays below the Ethernet MTU (1 500 - 20 IP
/// - 8 UDP = 1 472) with room for tunnelling headers.
inline constexpr std::size_t kDefaultMaxPacketSize = 1400;

namespace detail {

inline void put_be16(std::uint8_t* p, std::uint16_t v) noexcept {
  p[0] = static_cast<std::uint8_t>(v >> 8);
  p[1] = static_cast<std::uint8_t>(v);
}

inline void put_be64(std::uint8_t* p, std::uint64_t v) noexcept {
  for (int i = 7; i >= 0; --i) {
    p[i] = static_cast<std::uint8_t>(v);
    v >>= 8;
  }
}

/// Session + Sequence + Count: the layout shared by downstream and request packets.
inline void put_header(std::uint8_t* p, const Session& session, std::uint64_t seq, std::uint16_t count) noexcept {
  for (std::size_t i = 0; i < session.size(); ++i) {
    p[i] = static_cast<std::uint8_t>(session[i]);
  }
  put_be64(p + 10, seq);
  put_be16(p + 18, count);
}

}  // namespace detail

/// Heartbeat: header only, count 0; `next_seq` is the sequence number of the
/// next message the sender will send. Returns kHeaderSize, or 0 (nothing
/// written) when `out` is shorter than that.
inline std::size_t encode_heartbeat(std::span<std::uint8_t> out, const Session& session,
                                    std::uint64_t next_seq) noexcept {
  if (out.size() < kHeaderSize) {
    return 0;
  }
  detail::put_header(out.data(), session, next_seq, 0);
  return kHeaderSize;
}

/// End of session: header only, count 0xFFFF; `next_seq` as for a heartbeat.
inline std::size_t encode_end_of_session(std::span<std::uint8_t> out, const Session& session,
                                         std::uint64_t next_seq) noexcept {
  if (out.size() < kHeaderSize) {
    return 0;
  }
  detail::put_header(out.data(), session, next_seq, kEndOfSession);
  return kHeaderSize;
}

/// Builds one downstream packet at a time in a caller-provided buffer whose
/// size is the maximum packet size. Messages are appended in sequence order;
/// finish() writes the header and starts the next packet at the next
/// sequence number.
class PacketBuilder {
 public:
  enum class Add : std::uint8_t {
    Ok,          ///< appended
    Full,        ///< does not fit in this packet (bytes or count); finish() and retry
    TooLarge,    ///< cannot fit in any packet of this size, or longer than 65 535 bytes
    ZeroLength,  ///< a message of length 0 cannot be framed (the decoder rejects it)
  };

  /// Precondition: buffer.size() >= kHeaderSize.
  PacketBuilder(std::span<std::uint8_t> buffer, const Session& session, std::uint64_t first_seq) noexcept
      : buf_(buffer), session_(session), first_seq_(first_seq) {}

  /// Appends one message. Anything but Ok leaves the builder unchanged.
  Add add(itch::Bytes msg) noexcept {
    if (msg.empty()) {
      return Add::ZeroLength;
    }
    const std::size_t block = 2 + msg.size();
    if (msg.size() > 0xFFFF || block > buf_.size() - kHeaderSize) {
      return Add::TooLarge;
    }
    if (count_ == kMaxMessageCount || block > buf_.size() - size_) {
      return Add::Full;
    }
    detail::put_be16(buf_.data() + size_, static_cast<std::uint16_t>(msg.size()));
    std::copy(msg.begin(), msg.end(), buf_.begin() + static_cast<std::ptrdiff_t>(size_ + 2));
    size_ += block;
    ++count_;
    return Add::Ok;
  }

  /// Writes the header and returns the packet, which stays valid until the
  /// next add(). The builder then continues at next_seq(). Precondition: !empty().
  itch::Bytes finish() noexcept {
    detail::put_header(buf_.data(), session_, first_seq_, count_);
    const itch::Bytes packet{buf_.data(), size_};
    first_seq_ += count_;
    count_ = 0;
    size_ = kHeaderSize;
    return packet;
  }

  [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
  [[nodiscard]] std::uint16_t count() const noexcept { return count_; }
  /// Bytes of the packet so far, header included.
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] std::uint64_t first_seq() const noexcept { return first_seq_; }
  [[nodiscard]] std::uint64_t next_seq() const noexcept { return first_seq_ + count_; }

 private:
  std::span<std::uint8_t> buf_;
  Session session_;
  std::uint64_t first_seq_;
  std::uint16_t count_{0};
  std::size_t size_{kHeaderSize};
};

struct PacketizeResult {
  bool ok{true};
  std::size_t failed_index{0};  ///< first message that could not be framed (when !ok)
  PacketBuilder::Add reason{PacketBuilder::Add::Ok};
  std::uint64_t next_seq{0};  ///< sequence number after the last message sent
};

/// Splits `messages` (a range of byte spans, in sequence order starting at
/// `first_seq`) into packets of at most buffer.size() bytes, filling each
/// packet greedily, and calls on_packet(bytes) for each. Stops at the first
/// message that cannot be framed, after sending every message before it.
template <typename Range, typename F>
PacketizeResult packetize(const Range& messages, std::span<std::uint8_t> buffer, const Session& session,
                          std::uint64_t first_seq, F&& on_packet) {
  PacketBuilder b{buffer, session, first_seq};
  PacketizeResult r;
  std::size_t index = 0;
  for (const auto& m : messages) {
    const itch::Bytes msg{m};
    auto status = b.add(msg);
    if (status == PacketBuilder::Add::Full) {
      on_packet(b.finish());
      status = b.add(msg);
    }
    if (status != PacketBuilder::Add::Ok) {
      r.ok = false;
      r.failed_index = index;
      r.reason = status;
      break;
    }
    ++index;
  }
  if (!b.empty()) {
    on_packet(b.finish());
  }
  r.next_seq = b.next_seq();
  return r;
}

struct Request {
  Session session{};
  std::uint64_t sequence{0};  ///< first requested message
  std::uint16_t count{0};     ///< number of messages requested
};

enum class RequestStatus : std::uint8_t {
  Ok,
  Truncated,      ///< shorter than 20 bytes
  TrailingBytes,  ///< longer than 20 bytes
  ZeroCount,      ///< [project convention] a request for 0 messages is malformed
};

/// Returns kRequestSize, or 0 (nothing written) when `out` is shorter than that.
inline std::size_t encode_request(std::span<std::uint8_t> out, const Request& r) noexcept {
  if (out.size() < kRequestSize) {
    return 0;
  }
  detail::put_header(out.data(), r.session, r.sequence, r.count);
  return kRequestSize;
}

inline RequestStatus parse_request(itch::Bytes packet, Request& r) noexcept {
  if (packet.size() < kRequestSize) {
    return RequestStatus::Truncated;
  }
  if (packet.size() > kRequestSize) {
    return RequestStatus::TrailingBytes;
  }
  for (std::size_t i = 0; i < r.session.size(); ++i) {
    r.session[i] = static_cast<char>(packet[i]);
  }
  r.sequence = itch::be64(packet.data() + 10);
  r.count = itch::be16(packet.data() + 18);
  return r.count == 0 ? RequestStatus::ZeroCount : RequestStatus::Ok;
}

}  // namespace lob::md::mold
