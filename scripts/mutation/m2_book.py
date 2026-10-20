# Mutation check for M2 (order book). See mutate.py for the procedure.
# Usage: python3 scripts/mutation/m2_book.py "$PWD"   (run inside WSL/Linux)
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from mutate import run  # noqa: E402

SRC = "src/book/order_book.cpp"

MUTATIONS = [
    ("FIFO: new order joins the front of the queue", SRC, [
        ("level.orders.push_back(RestingOrder", "level.orders.push_front(RestingOrder"),
        (".order = std::prev(level.orders.end())", ".order = level.orders.begin()"),
        ("    level.orders.pop_back();\n", "    level.orders.pop_front();\n"),
    ]),
    ("cancel leaves an empty level behind", SRC, [
        ("    visit_side(loc.side, [&](auto& levels) { levels.erase(loc.price); });\n", ""),
    ]),
    ("fill_front forgets to reduce the level total", SRC, [
        ("    level.total_quantity -= quantity;\n", ""),
    ]),
    ("crossing check allows a locked book (>= becomes >)", SRC, [
        ("order.price >= *opposite_best : order.price <= *opposite_best",
         "order.price > *opposite_best : order.price < *opposite_best"),
    ]),
    ("fill_front rejects an exact complete fill", SRC, [
        ("quantity > maker.remaining)", "quantity >= maker.remaining)"),
    ]),
    ("rollback: new level not erased when the list insert throws", SRC, [
        ("  } catch (...) {\n    if (level_created) {\n      levels.erase(level_it);\n    }\n    throw;\n  }\n\n  try {",
         "  } catch (...) {\n    throw;\n  }\n\n  try {"),
    ]),
    ("rollback: list node not removed when the index insert throws", SRC, [
        ("    level.orders.pop_back();\n", ""),
    ]),
    ("move constructor leaves the source index populated", SRC, [
        ("      last_seq_(other.last_seq_) {\n  // Make the source state specified rather than library-dependent.\n  other.bids_.clear();\n  other.asks_.clear();\n  other.index_.clear();\n",
         "      last_seq_(other.last_seq_) {\n"),
    ]),
    ("depth returns one level too many", SRC, [
        ("if (result.size() == max_levels)", "if (result.size() > max_levels)"),
    ]),
    ("invariant checker skips the FIFO seq check", SRC, [
        ("if (previous_seq && order.seq <= *previous_seq)", "if (false && previous_seq)"),
    ]),
    ("for_each_order_while ignores the stop signal", "include/lob/book/order_book.hpp", [
        ("          if (!visit(price, order)) {\n            return;\n",
         "          if (!visit(price, order)) {\n            continue;\n"),
    ]),
]

run(sys.argv[1], MUTATIONS, "m2mut")
