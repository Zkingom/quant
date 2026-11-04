#pragma once

// MoldUDP64 downstream packet framing (L3' 1d). Used with hand-built packets
// to exercise real sequence-number gaps; the Nasdaq sample files are not
// MoldUDP64 (they carry only length prefixes).
//
// Packet (Nasdaq MoldUDP64 Protocol Specification v1.00):
//   Session          10  alpha
//   Sequence Number   8  integer, seq of the first message in the packet
//   Message Count     2  integer; 0 = heartbeat, 0xFFFF = end of session
//   then Message Count blocks of: Message Length 2 + Message Data

#include <array>
#include <cstdint>

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

}  // namespace lob::md::mold
