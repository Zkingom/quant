#pragma once

// M5: snapshot files (DESIGN.md §6.3, §6.5).
//
//   LOB-SNAPSHOT v1
//   CONFIG …
//   LAST_CMD <cmd_seq>      commit watermark: last command whose batch is included
//   LAST_SEQ <seq>
//   ORDERS <n>
//   ORDER <BUY|SELL> <price> <id> <remaining> <seq>   x n, canonical order
//   END <fnv1a64 hex of every byte before this line>

#include <cstdint>
#include <string>
#include <string_view>

#include "lob/engine/snapshot.hpp"
#include "lob/io/text_format.hpp"

namespace lob::io {

struct SnapshotFile {
  EngineSnapshot state;
  CmdSeq last_cmd = 0;

  friend bool operator==(const SnapshotFile&, const SnapshotFile&) = default;
};

std::string encode_snapshot(const SnapshotFile& snapshot);

/// Throws ParseError for malformed text, a wrong checksum, trailing bytes,
/// or a state rejected by find_snapshot_violation().
SnapshotFile parse_snapshot(std::string_view bytes);

/// Identity of an engine state (config, last_seq, every order incl. seq):
/// FNV-1a 64 of its canonical text. Equal states <=> equal hashes, up to
/// 64-bit collisions; used in summaries and as a quick equality check.
std::uint64_t state_hash(const EngineSnapshot& state);

}  // namespace lob::io
