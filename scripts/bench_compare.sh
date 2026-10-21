#!/usr/bin/env bash
# A/B comparison of two revisions under identical conditions: both are built
# (gcc-release) in separate git worktrees on ext4, then their benchmark runs
# are INTERLEAVED (A, B, A, B, ...) so slow drift of the machine affects both.
# Usage: scripts/bench_compare.sh REV_A REV_B RUNS OUT_DIR [lob_bench args...]
set -u
REV_A=$1; REV_B=$2; RUNS=$3; OUT=$4; shift 4
CPU=${BENCH_CPU:-2}
# Worktrees live under $HOME on ext4, not /tmp: WSL may restart an idle
# distro, which empties /tmp in the middle of a long comparison.
WORK=${BENCH_WORK:-$HOME/lob_bench_ab}
ROOT=$(git rev-parse --show-toplevel)
build() {  # build REV DIR
  rm -rf "$2"
  git -C "$ROOT" worktree add --detach "$2" "$1" > /dev/null 2>&1 || { echo "worktree $1 failed"; exit 1; }
  cmake -S "$2" -B "$2/b" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++ > /dev/null \
    && cmake --build "$2/b" --target lob_bench > /dev/null || { echo "build $1 failed"; exit 1; }
}
build "$REV_A" $WORK/A
build "$REV_B" $WORK/B
mkdir -p "$OUT/A" "$OUT/B"
{
  echo "A=$REV_A $(git -C "$ROOT" rev-parse --short "$REV_A")"
  echo "B=$REV_B $(git -C "$ROOT" rev-parse --short "$REV_B")"
  echo "date_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ) pinned_vcpu=$CPU args=$*"
  uname -srm; grep -m1 "model name" /proc/cpuinfo
} > "$OUT/compare_info.txt"
for i in $(seq 1 "$RUNS"); do
  taskset -c "$CPU" $WORK/A/b/bench/lob_bench "$@" > "$OUT/A/run_$i.csv" 2> "$OUT/A/run_$i.err" || exit 1
  taskset -c "$CPU" $WORK/B/b/bench/lob_bench "$@" > "$OUT/B/run_$i.csv" 2> "$OUT/B/run_$i.err" || exit 1
  echo "pair $i done"
done
git -C "$ROOT" worktree remove --force $WORK/A
git -C "$ROOT" worktree remove --force $WORK/B
