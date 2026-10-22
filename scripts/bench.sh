#!/usr/bin/env bash
# Runs lob_bench RUNS times as independent processes pinned to one vCPU and
# stores the raw CSV of each run (BENCHMARK.md §3).
# Usage: scripts/bench.sh LOB_BENCH OUT_DIR RUNS [lob_bench args...]
set -u
BIN=$1; OUT=$2; RUNS=$3; shift 3
CPU=${BENCH_CPU:-2}
mkdir -p "$OUT"
{
  echo "date_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "binary=$BIN"
  echo "args=$*"
  echo "pinned_vcpu=$CPU"
  uname -srm
  grep -m1 "model name" /proc/cpuinfo
  echo "load_before=$(cut -d' ' -f1-3 /proc/loadavg)"
} > "$OUT/run_info.txt"
for i in $(seq 1 "$RUNS"); do
  taskset -c "$CPU" "$BIN" "$@" > "$OUT/run_$i.csv" 2> "$OUT/run_$i.err" || { echo "run $i failed"; exit 1; }
  echo "run $i done ($(grep -c . "$OUT/run_$i.csv") lines)"
done
echo "load_after=$(cut -d' ' -f1-3 /proc/loadavg)" >> "$OUT/run_info.txt"
