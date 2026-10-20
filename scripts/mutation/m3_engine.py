# Mutation check for M3 (matching engine). See mutate.py for the procedure.
# Usage: python3 scripts/mutation/m3_engine.py "$PWD"   (run inside WSL/Linux)
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from mutate import run  # noqa: E402

SRC = "src/engine/matching_engine.cpp"

MUTATIONS = [
    ("trade executes at the taker's limit instead of the maker's price", SRC, [
        ("                                   .price = fill.price,",
         "                                   .price = order.price,"),
    ]),
    ("equal prices do not trade", SRC, [
        ("maker_price <= limit : maker_price >= limit", "maker_price < limit : maker_price > limit"),
    ]),
    # Keeps accepted_seq referenced so -Werror (unused variable) does not kill
    # the mutant at build time: the tests must catch it.
    ("priority taken from the RESTED seq instead of the ACCEPTED seq", SRC, [
        ("              accepted_seq);", "              (static_cast<void>(accepted_seq), seq));"),
    ]),
    ("BOOK_FULL ignores slots freed by complete fills", SRC, [
        ("book_.order_count() - plan.makers_filled", "book_.order_count()"),
    ]),
    ("BOOK_FULL check removed", SRC, [
        ("      reason = RejectReason::BookFull;", "      (void)0;"),
    ]),
    ("duplicate-id check removed", SRC, [
        ("    reason = RejectReason::DuplicateOrderId;", "    (void)0;"),
    ]),
    ("remainder never rests", SRC, [
        ("  if (remaining == 0) {\n    return;\n  }", "  if (remaining >= 0) {\n    return;\n  }"),
    ]),
    ("fill quantity is the maker's whole remaining", SRC, [
        ("    const Quantity quantity = std::min(remaining, maker->remaining);",
         "    const Quantity quantity = maker->remaining;"),
    ]),
    ("plan keeps counting after the order is filled", SRC, [
        ("    return remaining > 0;\n  });", "    return true;\n  });"),
    ]),
    ("resource failure does not report the cancelled remainder", SRC, [
        ("  } catch (const std::bad_alloc&) {\n    events.push_back(OrderCancelled{",
         "  } catch (const std::bad_alloc&) {\n    if (false) events.push_back(OrderCancelled{"),
    ]),
    ("event capacity not reserved before the commit", SRC, [
        ("  SeqNum seq = first_seq_of_block(event_count);\n  ensure_capacity(events, event_count);",
         "  SeqNum seq = first_seq_of_block(event_count);"),
    ]),
    ("RESTED reuses the last trade's seq", SRC, [
        ("  ++seq;\n  try {", "  try {"),
    ]),
    ("unknown cancel reported as INVALID_ORDER_ID", SRC, [
        ("    reason = RejectReason::UnknownOrderId;", "    reason = RejectReason::InvalidOrderId;"),
    ]),
    ("rejection does not advance last_seq", SRC, [
        ("  events.push_back(OrderRejected{.seq = seq, .command = command, .id = id, .reason = reason});\n  last_seq_ = seq;",
         "  events.push_back(OrderRejected{.seq = seq, .command = command, .id = id, .reason = reason});"),
    ]),
]

run(sys.argv[1], MUTATIONS, "m3mut")
