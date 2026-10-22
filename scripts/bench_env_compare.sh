#!/usr/bin/env bash
# A/B comparison of ONE binary under two environments: A = unchanged, B = with
# the variable assignment ENV_B (e.g. GLIBC_TUNABLES=glibc.malloc.tcache_count=65535).
# Runs are interleaved (A, B, A, B, ...) and pinned like scripts/bench.sh.
# Usage: scripts/bench_env_compare.sh LOB_BENCH RUNS OUT_DIR ENV_B [lob_bench args...]
set -u
BIN=$1; RUNS=$2; OUT=$3; ENV_B=$4; shift 4
CPU=${BENCH_CPU:-2}
mkdir -p "$OUT/A" "$OUT/B"
{
  echo "binary=$BIN"
  echo "A=(environment unchanged) B=$ENV_B"
  echo "date_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ) pinned_vcpu=$CPU args=$*"
  uname -srm; grep -m1 "model name" /proc/cpuinfo; ldd --version | head -1
  echo "load_before=$(cut -d' ' -f1-3 /proc/loadavg)"
} > "$OUT/compare_info.txt"
for i in $(seq 1 "$RUNS"); do
  taskset -c "$CPU" "$BIN" "$@" > "$OUT/A/run_$i.csv" 2> "$OUT/A/run_$i.err" || exit 1
  env "$ENV_B" taskset -c "$CPU" "$BIN" "$@" > "$OUT/B/run_$i.csv" 2> "$OUT/B/run_$i.err" || exit 1
  echo "pair $i done"
done
echo "load_after=$(cut -d' ' -f1-3 /proc/loadavg)" >> "$OUT/compare_info.txt"
