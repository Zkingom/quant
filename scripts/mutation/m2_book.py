# Mutation check for M2: apply one seeded bug at a time to an isolated copy
# of the source tree, rebuild, run all tests, report which tests fail.
# Usage: python3 scripts/mutation/m2_book.py "$PWD"   (run inside WSL/Linux)
import pathlib
import shutil
import subprocess
import sys

REPO = pathlib.Path(sys.argv[1])
WORK = pathlib.Path("/tmp/m2mut")
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
]


def run(cmd, cwd):
    return subprocess.run(cmd, cwd=cwd, shell=True, capture_output=True, text=True)


def main():
    if WORK.exists():
        shutil.rmtree(WORK)
    WORK.mkdir()
    for item in ["CMakeLists.txt", "cmake", "include", "src", "tests"]:
        src = REPO / item
        (shutil.copytree if src.is_dir() else shutil.copy)(src, WORK / item)
    configure = run("cmake -S . -B b -G Ninja -DLOB_BUILD_EXAMPLES=OFF -DCMAKE_BUILD_TYPE=Debug", WORK)
    assert configure.returncode == 0, configure.stdout + configure.stderr

    survivors = 0
    for name, rel, edits in MUTATIONS:
        path = WORK / rel
        original = path.read_text()
        mutated = original
        for old, new in edits:
            count = mutated.count(old)
            if count != 1:
                print(f"[ERROR] '{name}': pattern found {count} times: {old!r}")
                sys.exit(2)
            mutated = mutated.replace(old, new)
        path.write_text(mutated)
        build = run("cmake --build b", WORK)
        if build.returncode != 0:
            print(f"[KILLED-BUILD] {name}")
        else:
            test = run("ctest --test-dir b", WORK)
            failed = [l.split(" - ", 1)[1].split(" (")[0] for l in test.stdout.splitlines()
                      if " - " in l and "(Failed" in l or "(Subprocess" in l]
            if test.returncode == 0:
                survivors += 1
                print(f"[SURVIVED] {name}")
            else:
                print(f"[KILLED] {name}: {len(failed)} failing: {', '.join(failed[:6])}"
                      + (" ..." if len(failed) > 6 else ""))
        path.write_text(original)

    # Sanity: the unmutated tree passes.
    build = run("cmake --build b", WORK)
    test = run("ctest --test-dir b", WORK)
    print(f"[BASELINE] unmutated tree: build rc={build.returncode} ctest rc={test.returncode}")
    print(f"survivors: {survivors}/{len(MUTATIONS)}")


main()
