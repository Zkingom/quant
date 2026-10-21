# Mutation check for M5 (replay, journal, recovery, session). See mutate.py.
# Usage: python3 scripts/mutation/m5_replay.py "$PWD"   (run inside WSL/Linux)
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from mutate import run  # noqa: E402

SESSION = "src/io/session.cpp"
JOURNAL = "src/io/journal.cpp"
RECOVERY = "src/io/recovery.cpp"

MUTATIONS = [
    ("session: commit record allocates after the commit point (pre-fix code)", SESSION, [
        ("  committed_.push_back(CommittedCommand{.digest = digest,",
         "  committed_.push_back(CommittedCommand{.digest = command_digest(command),"),
    ]),
    ("session: duplicate detection disabled", SESSION, [
        ("  if (command.cmd_seq >= 1 && command.cmd_seq <= last_cmd_) {",
         "  if (false && command.cmd_seq >= 1 && command.cmd_seq <= last_cmd_) {"),
    ]),
    ("session: content check of duplicates disabled", SESSION, [
        ("      if (c.digest != digest) {", "      if (false) {"),
    ]),
    ("session: journal write failure ignored (acknowledges uncommitted results)", SESSION, [
        ('    return poison("journal write failed (command applied in memory, not committed): ", e.what());',
         "    (void)e;"),
    ]),
    ("session: durable mode skips fsync", SESSION, [
        ("    sink_.write(batch);\n    if (options_.durable) {", "    sink_.write(batch);\n    if (false) {"),
    ]),
    ("journal: checksum not verified", JOURNAL, [
        ("    if (f[2] != expected_hash) {", "    if (false && f[2] != expected_hash) {"),
    ]),
    ("journal: event seq contiguity not checked", JOURNAL, [
        ("      if (seq != expected_seq) {", "      if (false && seq != expected_seq) {"),
    ]),
    ("journal: valid length not advanced (restart would cut committed batches)", JOURNAL, [
        ("      scan.valid_bytes = batch->end_offset;\n", ""),
    ]),
    ("journal: a torn tail is accepted as a complete batch boundary", JOURNAL, [
        ("      scan.tail = TailState::Torn;\n      scan.dropped_bytes = bytes.size() - scan.valid_bytes;\n      return scan;",
         "      scan.tail = TailState::Clean;\n      scan.valid_bytes = bytes.size();\n      return scan;"),
    ]),
    ("recovery: snapshot/journal boundary not checked", RECOVERY, [
        ("    if (boundary.last_seq() != snapshot->state.last_seq) {",
         "    if (false && boundary.last_seq() != snapshot->state.last_seq) {"),
    ]),
    ("recovery: rested order gets the RESTED seq as priority", RECOVERY, [
        ("      resting_.emplace(o.id, Order{o.side, o.price, remaining, a->seq});",
         "      resting_.emplace(o.id, Order{o.side, o.price, remaining, rested->seq});"),
    ]),
    ("recovery: trades do not reduce the maker", RECOVERY, [
        ("    maker->second.remaining -= t.quantity;\n", ""),
    ]),
    ("restore: orders re-added in descending seq order", "src/engine/matching_engine.cpp", [
        ("[](const SnapshotOrder& a, const SnapshotOrder& b) { return a.seq < b.seq; }",
         "[](const SnapshotOrder& a, const SnapshotOrder& b) { return a.seq > b.seq; }"),
    ]),
    ("text format: leading zeros accepted", "src/io/text_format.cpp", [
        ("  if (token.size() > 1 && token[0] == '0') {", "  if (false && token.size() > 1 && token[0] == '0') {"),
    ]),
    ("file sink: torn tail not truncated before appending", "src/io/file_sink.cpp", [
        ("  if (::ftruncate(fd_, static_cast<off_t>(keep_bytes)) != 0 ||", "  if (false ||"),
    ]),
]

run(sys.argv[1], MUTATIONS, "m5mut")
