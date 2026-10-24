#!/usr/bin/env bash
# L2: reproduces the whole baseline-vs-optimized comparison with one command
# (ROADMAP §2.2): builds both implementations (LOB_BASELINE=ON / OFF), checks
# their equivalence, compares throughput and latency, measures peak RSS, and
# writes everything plus a summary to OUT_DIR. Run inside WSL/Linux from any
# directory.
#
# Usage: scripts/l2_reproduce.sh [OUT_DIR]      (default MODULE_NOTES/evidence/L2/reproduce)
#   QUICK=1 scripts/l2_reproduce.sh /tmp/x      smoke run: 1 pair, short streams (not a result)
# Environment: CXX (default g++), BENCH_CPU (default 2), RUNS (default 5).
#
# Stages and the documents they reproduce:
#   1. build      build/l2-optimized, build/l2-baseline (README "LOB_BASELINE")
#   2. equivalence scripts/l2_equivalence.sh             (MODULE_NOTES/L2_EQUIVALENCE.md)
#   3. bench      lob_bench of both builds, interleaved,
#                 --filter session --reps 3 and --filter engine.mixed  (BENCHMARK §9)
#   4. rss        scripts/l2_rss.sh                       (BENCHMARK §12)
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
OUT=${1:-MODULE_NOTES/evidence/L2/reproduce}
QUICK=${QUICK:-0}
CXX=${CXX:-g++}
CPU=${BENCH_CPU:-2}
if [ "$QUICK" = "1" ]; then
  RUNS=1; BENCH_N=(--engine-commands 20000 --durable-commands 200); EQ_N=2000; RSS_N=20000
else
  RUNS=${RUNS:-5}; BENCH_N=(); EQ_N=100000; RSS_N=200000
fi
OPT=build/l2-optimized
BASE=build/l2-baseline
die() { echo "l2_reproduce: FAILED: $*" >&2; exit 1; }
stage() { echo "== [$(date -u +%H:%M:%S)] $*"; }

rm -rf "$OUT"
mkdir -p "$OUT"
start=$(date +%s)

stage "1/4 build (CXX=$CXX, from empty build directories)"
# Always from scratch: reconfiguring an existing directory with a different
# compiler makes CMake delete its cache and silently drop -DLOB_BASELINE=ON
# (seen while writing this script), leaving a "baseline" directory that is an
# optimized build.
for v in OFF ON; do
  dir=$OPT; [ $v = ON ] && dir=$BASE
  rm -rf "$dir"
  cmake -S . -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="$CXX" -DLOB_BASELINE=$v \
    > "$OUT/build_$v.log" 2>&1 && cmake --build "$dir" >> "$OUT/build_$v.log" 2>&1 || die "build LOB_BASELINE=$v (see $OUT/build_$v.log)"
  [ "$(sed -n 's/^LOB_BASELINE:BOOL=//p' "$dir/CMakeCache.txt")" = "$v" ] || die "$dir: LOB_BASELINE is not $v after configuring"
done

stage "2/4 equivalence"
bash scripts/l2_equivalence.sh "$OPT" "$BASE" "$OUT/equivalence.md" "$EQ_N" > "$OUT/equivalence.log" 2>&1
eq_rc=$?
[ $eq_rc -le 1 ] || die "equivalence script error (see $OUT/equivalence.log)"

stage "3/4 bench ($RUNS interleaved pairs per filter, vCPU $CPU)"
bench_pairs() {  # NAME ARGS...
  local name=$1; shift
  mkdir -p "$OUT/bench_$name/A" "$OUT/bench_$name/B"
  for i in $(seq 1 "$RUNS"); do
    taskset -c "$CPU" "$BASE/bench/lob_bench" "$@" > "$OUT/bench_$name/A/run_$i.csv" 2> "$OUT/bench_$name/A/run_$i.err" \
      || die "baseline bench $name run $i"
    taskset -c "$CPU" "$OPT/bench/lob_bench" "$@" > "$OUT/bench_$name/B/run_$i.csv" 2> "$OUT/bench_$name/B/run_$i.err" \
      || die "optimized bench $name run $i"
    echo "   $name pair $i done"
  done
  python3 -I scripts/bench_report.py "$OUT/bench_$name/A" "$OUT/bench_$name/B" > "$OUT/bench_$name/report.md" \
    || die "bench_report $name"
}
bench_pairs session --filter session --reps 3 "${BENCH_N[@]}"
bench_pairs engine_control --filter engine.mixed "${BENCH_N[@]}"
for d in "$OUT"/bench_*/A "$OUT"/bench_*/B; do
  grep -q '^env,lob_baseline,0,1$' "$d/run_1.csv" && want=A || want=B
  [ "$(basename "$d")" = "$want" ] || die "$d was produced by the wrong build"
done

stage "4/4 rss"
bash scripts/l2_rss.sh "$OPT" "$BASE" "$OUT/rss" "$RUNS" "$RSS_N" mixed deep_book > "$OUT/rss.log" 2>&1 || die "rss (see $OUT/rss.log)"

stage "summary"
rss_table=$(python3 -I - "$OUT/rss/rss.csv" <<'PY'
import csv, statistics, sys
rows = [r for r in csv.reader(open(sys.argv[1])) if r and not r[0].startswith("#") and r[0] != "profile"]
for p in dict.fromkeys(r[0] for r in rows):
    k = {b: [int(r[3]) for r in rows if r[0] == p and r[1] == b] for b in ("base", "opt")}
    mb, mo = statistics.median(k["base"]), statistics.median(k["opt"])
    print(f"| {p} | {mb:.0f} ({min(k['base'])}–{max(k['base'])}) | {mo:.0f} ({min(k['opt'])}–{max(k['opt'])}) "
          f"| {mo - mb:+.0f} ({(mo - mb) / mb * 100:+.1f}%) |")
PY
)
{
  echo "# L2 复现汇总（基线 vs 优化）"
  echo
  echo "由 \`scripts/l2_reproduce.sh\` 生成。$( [ "$QUICK" = "1" ] && echo '**QUICK=1：冒烟运行，数字不是结果。**')"
  echo
  echo "- 时间：$(date -u +%Y-%m-%dT%H:%M:%SZ)，耗时 $(( $(date +%s) - start )) s；源码 \`$(git rev-parse --short HEAD)\`$(git diff --quiet HEAD -- src include apps testkit bench scripts || echo '（工作区有未提交改动）')"
  echo "- $(uname -srm)；$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ //')；编译器 $($CXX --version | head -1)；Release"
  echo "- 构建：基线 \`$BASE\`（\`LOB_BASELINE=ON\`）、优化 \`$OPT\`（\`OFF\`）；基准固定 vCPU $CPU；每项 $RUNS 次（交替）"
  echo
  echo "## 1. 等价性（\`equivalence.md\`）"
  echo
  grep '^\*\*结论' "$OUT/equivalence.md"
  echo
  echo "## 2. 吞吐与延迟（A = 基线，B = 优化；\`bench_*/report.md\`，原始 CSV 在 \`bench_*/A|B/\`）"
  echo
  echo "### session（\`--filter session --reps 3\`）"
  echo
  sed -n '3,$p' "$OUT/bench_session/report.md"
  echo
  echo "### engine.mixed 对照（撮合引擎代码在两种构建中相同）"
  echo
  sed -n '3,$p' "$OUT/bench_engine_control/report.md" | grep -v '\[trades'
  echo
  echo "## 3. 峰值 RSS（\`rss/rss.csv\`，KiB，中位数（范围））"
  echo
  echo "| 画像 | 基线 | 优化 | 优化 − 基线 |"
  echo "|---|---|---|---|"
  echo "$rss_table"
  echo
  echo "数字只适用于本次运行的机器、构建与负载；解读见 BENCHMARK §9、§12。"
} > "$OUT/summary.md"

stage "done in $(( $(date +%s) - start )) s -> $OUT/summary.md"
[ $eq_rc -eq 0 ] || die "the two implementations are NOT equivalent (see $OUT/equivalence.md)"
