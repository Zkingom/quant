#pragma once

// Frame + decode a contiguous byte buffer into FeedInputs (L3' 1d/2): the
// single-threaded decode loop shared by file replay, the decoder thread of
// the pipeline, and the tests. Header-only so that the sink call inlines.

#include <cstdint>

#include "lob/md/feed.hpp"
#include "lob/md/itch.hpp"

namespace lob::md {

struct StreamProgress {
  std::size_t consumed{0};       ///< bytes fully consumed (complete frames)
  std::uint64_t next_seq{0};     ///< seq to assign to the next frame
  itch::FrameStatus last{itch::FrameStatus::End};  ///< End or Truncated (need more) or ZeroLength
};

/// Calls sink(const FeedInput&) once per complete frame of `buffer`, in
/// order, with seq = first_seq, first_seq + 1, ... Stops at the end of the
/// buffer, at an incomplete trailing frame (Truncated: carry the tail into
/// the next chunk, or report a FrameError at end of input), or at a
/// zero-length prefix (unrecoverable: the caller reports FrameError).
template <typename Sink>
StreamProgress decode_frames(itch::Bytes buffer, std::uint64_t first_seq, Sink&& sink) {
  itch::FrameCursor cursor{buffer};
  std::uint64_t seq = first_seq;
  itch::Bytes payload;
  FeedInput in;
  for (;;) {
    const itch::FrameStatus fs = cursor.next(payload);
    if (fs != itch::FrameStatus::Frame) {
      return StreamProgress{cursor.offset(), seq, fs};
    }
    in.seq = seq++;
    const itch::DecodeStatus ds = itch::decode(payload, in.msg);
    if (ds == itch::DecodeStatus::Decoded) {
      in.kind = InputKind::Decoded;
      in.error = 0;
    } else if (ds == itch::DecodeStatus::Skipped) {
      in.kind = InputKind::Skipped;
      in.error = 0;
    } else {
      in.kind = InputKind::DecodeError;
      in.error = static_cast<std::uint8_t>(ds);
    }
    sink(static_cast<const FeedInput&>(in));
  }
}

}  // namespace lob::md
