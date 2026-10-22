#!/usr/bin/env bash
# Re-runs the M6 profiling steps (MODULE_NOTES/M6_performance.md §5) against a
# Release build with -g and writes the reports into OUT_DIR.
# Usage: scripts/profile.sh LOB_BENCH OUT_DIR
# Needs: valgrind (callgrind), linux perf (cpu-clock sampling and `perf trace`).
# On WSL2 the perf binary matching the kernel is usually missing from PATH; set
# PERF=/usr/lib/linux-tools/<version>/perf.
set -u
BIN=$1; OUT=$2
PERF=${PERF:-perf}
CPU=${BENCH_CPU:-2}
mkdir -p "$OUT"
W=$(mktemp -d "$HOME/lob_profile.XXXXXX")
tidy() { sed -e "s/std::__cxx11:://g" -e "s#/usr/include/c++/[0-9]*/##" | cut -c1-230 | head -n 140; }

# 1. Session throughput vs stream length (shape of the cost per command).
{
  echo "# session.memory throughput vs stream length ($BIN, vCPU $CPU)"
  for n in 20000 40000 80000 160000; do
    printf 'n=%-7s ' "$n"
    taskset -c "$CPU" "$BIN" --filter session.memory --reps 1 --engine-commands "$n" | grep throughput
  done
} > "$OUT/session_scaling.txt"

# 2. Where Session::submit spends wall time (cpu-clock samples).
"$PERF" record -e cpu-clock -F 999 -o "$W/session.data" -- \
  taskset -c "$CPU" "$BIN" --filter session.memory --reps 1 --engine-commands 80000 > /dev/null 2>&1
"$PERF" report -i "$W/session.data" --no-children --sort symbol --percent-limit 1 --stdio 2>/dev/null \
  | grep -vE "^#|^$" | tidy > "$OUT/perf_session_memory.txt"

# 3. Instruction profile of MatchingEngine::process (collection toggled on the
#    variant overload only: nested matches would switch collection off again).
for p in mixed deep_book; do
  valgrind --tool=callgrind --callgrind-out-file="$W/cg.$p" \
    "--toggle-collect=lob::MatchingEngine::process(std::variant*" \
    "$BIN" --filter "engine.$p" --reps 1 --engine-commands 20000 > /dev/null 2>&1
  callgrind_annotate "$W/cg.$p" 2>/dev/null | tidy > "$OUT/callgrind_engine_${p}_self.txt"
  callgrind_annotate --inclusive=yes "$W/cg.$p" 2>/dev/null | tidy > "$OUT/callgrind_engine_${p}_inclusive.txt"
done

# 4. Book add/cancel at depth 100k with simulated caches and branch predictor.
valgrind --tool=callgrind --cache-sim=yes --branch-sim=yes --callgrind-out-file="$W/cg.book" \
  "--toggle-collect=lob::OrderBook::add(*" "--toggle-collect=lob::OrderBook::cancel(*" \
  "$BIN" --filter book.add_cancel@100000 --reps 1 > /dev/null 2>&1
callgrind_annotate --show=Ir,D1mr,DLmr,Bcm --sort=Ir "$W/cg.book" 2>/dev/null | tidy \
  > "$OUT/callgrind_book_add_cancel_100k_cachesim.txt"

# 5. System calls of the file-backed sessions.
for s in session.ext4_buffered session.ext4_fsync; do
  "$PERF" trace -s -o "$W/trace.$s" -- taskset -c "$CPU" "$BIN" --filter "$s" --reps 1 --engine-commands 20000 \
    > "$W/trace.$s.csv" 2> /dev/null
  { echo "# perf trace -s, $BIN --filter $s --reps 1 --engine-commands 20000; warmup + 1 rep + latency pass = 3 sessions"
    echo "# syscall times include tracing overhead and are NOT used as performance results"
    grep -E "throughput|samples" "$W/trace.$s.csv"; cat "$W/trace.$s"; } > "$OUT/syscalls_$s.txt"
done
rm -rf "$W"
