#pragma once

// Hand-built ITCH 5.0 binary samples for tests (L3' 1b/1e). An encoder that
// is written independently of the decoder: it appends fields in
// specification order with push_back, while the decoder reads them at fixed
// offsets, so a shared offset mistake would have to be made twice in two
// different styles. The decoder tests additionally pin one message of each
// type to a literal byte string written out by hand from the specification.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <utility>
#include <vector>

namespace lob::testkit::itch {

using Buf = std::vector<std::uint8_t>;

class MsgWriter {
 public:
  MsgWriter(char type, std::uint16_t locate, std::uint16_t tracking, std::uint64_t ts) {
    c(type);
    u16(locate);
    u16(tracking);
    u48(ts);
  }
  MsgWriter& c(char v) {
    b_.push_back(static_cast<std::uint8_t>(v));
    return *this;
  }
  MsgWriter& u16(std::uint16_t v) { return be(v, 2); }
  MsgWriter& u32(std::uint32_t v) { return be(v, 4); }
  MsgWriter& u48(std::uint64_t v) { return be(v, 6); }
  MsgWriter& u64(std::uint64_t v) { return be(v, 8); }
  /// Alpha field of width n: left-justified, space-padded (truncated if longer).
  MsgWriter& alpha(std::string_view s, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
      c(i < s.size() ? s[i] : ' ');
    }
    return *this;
  }
  [[nodiscard]] Buf take() { return std::move(b_); }

 private:
  MsgWriter& be(std::uint64_t v, int bytes) {
    for (int i = bytes - 1; i >= 0; --i) {
      b_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xffU));
    }
    return *this;
  }
  Buf b_;
};

// Payload builders (no length prefix). Defaults keep test code short.
inline Buf system_event(char code, std::uint64_t ts = 0, std::uint16_t tracking = 0) {
  return MsgWriter('S', 0, tracking, ts).c(code).take();
}

inline Buf stock_directory(std::uint16_t locate, std::string_view stock, std::uint64_t ts = 0) {
  return MsgWriter('R', locate, 0, ts)
      .alpha(stock, 8)
      .c('Q')        // market category
      .c('N')        // financial status
      .u32(100)      // round lot size
      .c('N')        // round lots only
      .c('C')        // issue classification
      .alpha("Z", 2) // issue subtype
      .c('P')        // authenticity
      .c('N')        // short sale threshold
      .c(' ')        // IPO flag
      .c('1')        // LULD tier
      .c('N')        // ETP flag
      .u32(0)        // ETP leverage
      .c('N')        // inverse
      .take();
}

inline Buf add_order(std::uint16_t locate, std::uint64_t ref, char side, std::uint32_t shares,
                     std::string_view stock, std::uint32_t price, std::uint64_t ts = 0) {
  return MsgWriter('A', locate, 0, ts).u64(ref).c(side).u32(shares).alpha(stock, 8).u32(price).take();
}

inline Buf add_order_mpid(std::uint16_t locate, std::uint64_t ref, char side, std::uint32_t shares,
                          std::string_view stock, std::uint32_t price, std::string_view mpid,
                          std::uint64_t ts = 0) {
  return MsgWriter('F', locate, 0, ts)
      .u64(ref).c(side).u32(shares).alpha(stock, 8).u32(price).alpha(mpid, 4).take();
}

inline Buf order_executed(std::uint16_t locate, std::uint64_t ref, std::uint32_t shares,
                          std::uint64_t match, std::uint64_t ts = 0) {
  return MsgWriter('E', locate, 0, ts).u64(ref).u32(shares).u64(match).take();
}

inline Buf order_executed_price(std::uint16_t locate, std::uint64_t ref, std::uint32_t shares,
                                std::uint64_t match, char printable, std::uint32_t price,
                                std::uint64_t ts = 0) {
  return MsgWriter('C', locate, 0, ts).u64(ref).u32(shares).u64(match).c(printable).u32(price).take();
}

inline Buf order_cancel(std::uint16_t locate, std::uint64_t ref, std::uint32_t shares,
                        std::uint64_t ts = 0) {
  return MsgWriter('X', locate, 0, ts).u64(ref).u32(shares).take();
}

inline Buf order_delete(std::uint16_t locate, std::uint64_t ref, std::uint64_t ts = 0) {
  return MsgWriter('D', locate, 0, ts).u64(ref).take();
}

inline Buf order_replace(std::uint16_t locate, std::uint64_t old_ref, std::uint64_t new_ref,
                         std::uint32_t shares, std::uint32_t price, std::uint64_t ts = 0) {
  return MsgWriter('U', locate, 0, ts).u64(old_ref).u64(new_ref).u32(shares).u32(price).take();
}

/// A spec-valid message outside the subset: Trade (non-cross), 44 bytes.
inline Buf trade_message(std::uint16_t locate, std::uint64_t ts = 0) {
  return MsgWriter('P', locate, 0, ts)
      .u64(0).c('B').u32(100).alpha("TEST", 8).u32(10000).u64(1).take();
}

/// Appends `payload` with its 2-byte big-endian length prefix.
inline void append_frame(Buf& stream, const Buf& payload) {
  stream.push_back(static_cast<std::uint8_t>((payload.size() >> 8U) & 0xffU));
  stream.push_back(static_cast<std::uint8_t>(payload.size() & 0xffU));
  stream.insert(stream.end(), payload.begin(), payload.end());
}

inline Buf frames(std::initializer_list<Buf> payloads) {
  Buf out;
  for (const Buf& p : payloads) {
    append_frame(out, p);
  }
  return out;
}

}  // namespace lob::testkit::itch
