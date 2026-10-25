#!/usr/bin/env bash
# L2: measured peak resident set size (ru_maxrss) of the optimized build
# (LOB_BASELINE=OFF) vs the baseline build (LOB_BASELINE=ON) on the same
# command files. Each run is `lob_replay run` (journal on ext4 under $HOME,
# non-durable) measured with GNU time (`/usr/bin/time -f %M`, the child's
# ru_maxrss in KiB). Not python's getrusage(RUSAGE_CHILDREN): ru_maxrss keeps
# the pre-exec maximum, and a child forked from python starts at python's own
# ~10 MB (measured: /bin/true reports 11.2 MB that way, 1.1 MB via GNU time).
# The two builds are run alternately (A, B, A, B, ...). Raw rows go to
# OUT_DIR/rss.csv.
#
# Usage: scripts/l2_rss.sh OPT_BUILD_DIR BASE_BUILD_DIR OUT_DIR [RUNS] [COMMANDS] [PROFILES...]
#   defaults: RUNS=5, COMMANDS=200000, PROFILES="mixed deep_book"
set -u
OPT=$1; BASE=$2; OUT=$3; RUNS=${4:-5}; N=${5:-200000}; shift $(( $# < 5 ? $# : 5 ))
PROFILES=${*:-mixed deep_book}
W=$(mktemp -d "$HOME/lob_l2_rss.XXXXXX")
die() { echo "l2_rss: $*" >&2; rm -rf "$W"; exit 2; }
flag() { sed -n 's/^LOB_BASELINE:BOOL=//p' "$1/CMakeCache.txt"; }
[ "$(flag "$OPT")" = "OFF" ] || die "$OPT is not a LOB_BASELINE=OFF build"
[ "$(flag "$BASE")" = "ON" ] || die "$BASE is not a LOB_BASELINE=ON build"
mkdir -p "$OUT"

# rss BUILD INPUT -> "max_rss_kib wall_seconds"
rss() {
  rm -f "$W/j" "$W/t"
  /usr/bin/time -o "$W/t" -f "%M %e" "$1/apps/lob_replay" run --commands "$2" --journal "$W/j" \
    > /dev/null 2> "$W/err" || { echo "lob_replay failed: $(cat "$W/err")" >&2; return 1; }
  cat "$W/t"
}
[ -x /usr/bin/time ] || die "GNU time (/usr/bin/time) is required"
/usr/bin/time -o "$W/t" -f "%M" /bin/true
FLOOR=$(cat "$W/t")

{
  echo "# date_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ) rev=$(git -C "$(dirname "$0")/.." rev-parse --short HEAD)"
  echo "# opt=$OPT base=$BASE runs=$RUNS commands=$N profiles=$PROFILES"
  echo "# $(uname -srm); $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ //'); MemTotal $(awk '/MemTotal/{print $2}' /proc/meminfo) kB"
  echo "# journal: $W (filesystem $(df -T "$W" | tail -1 | awk '{print $2}')); wall_seconds is context only, not a benchmark result"
  echo "# measurement floor: /bin/true under the same GNU time = $FLOOR KiB"
  echo "profile,build,run,max_rss_kib,wall_seconds,input_bytes"
} > "$OUT/rss.csv"

for p in $PROFILES; do
  in="$W/$p.cmd"
  "$OPT/testkit/lob_gen_commands" --profile "$p" --seed 1 --commands "$N" > "$in" || die "generator failed for $p"
  bytes=$(stat -c %s "$in")
  for i in $(seq 1 "$RUNS"); do
    for b in opt base; do
      dir=$OPT; [ $b = base ] && dir=$BASE
      read -r kib secs <<< "$(rss "$dir" "$in")" || die "run failed ($p $b $i)"
      [ -n "$kib" ] || die "no ru_maxrss ($p $b $i)"
      echo "$p,$b,$i,$kib,$secs,$bytes" >> "$OUT/rss.csv"
      echo "$p $b run $i: ${kib} KiB (${secs} s)"
    done
  done
done
rm -rf "$W"
