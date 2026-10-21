#!/usr/bin/env bash
# Regenerates MODULE_NOTES/evidence/M5/ from a clean build (run inside WSL/Linux).
# Every stage must succeed; an incomplete stage makes the script exit non-zero.
# Usage: scripts/evidence_m5.sh
#   RESUME_FROM_STAGE=3 scripts/evidence_m5.sh   keeps the stage 1-2 logs and the
#   build from a previous run (use only when nothing but scripts changed since).
set -u
cd "$(dirname "$0")/.."
E=MODULE_NOTES/evidence/M5
FROM=${RESUME_FROM_STAGE:-1}
die() { echo "EVIDENCE FAILED: $*"; exit 1; }
if [ "$FROM" -le 1 ]; then
rm -rf "$E"
mkdir -p "$E/soak"

{
  echo "# captured: $(date -u +%Y-%m-%dT%H:%M:%SZ)  base commit: $(git rev-parse --short HEAD) + M5 working tree (committed together with this file)"
  uname -srm; head -1 /etc/os-release; nproc | sed 's/^/cpus: /'
  grep -m1 "model name" /proc/cpuinfo; grep MemTotal /proc/meminfo
  df -T /tmp | tail -1 | awk '{print "/tmp filesystem: " $2}'
  df -T . | tail -1 | awk '{print "source tree filesystem: " $2}'
  g++ --version | head -1; clang++ --version | head -1; cmake --version | head -1; echo "ninja $(ninja --version)"
} > "$E/environment.txt"

# 1. Full regression, all six presets, from scratch (includes CLI golden and crash tests).
rm -rf build
start=$(date +%s)
bash scripts/check.sh > "$E/check_all_presets.log" 2>&1
rc=$?
echo "check.sh rc=$rc wall_seconds=$(( $(date +%s) - start ))" >> "$E/check_all_presets.log"
[ $rc -eq 0 ] || die "check.sh"
fi

B=$PWD/build/gcc-release   # absolute: the CLI scripts change directory
if [ "$FROM" -le 2 ]; then
# 2. Verbatim outputs of the M5 acceptance tests and the M4 anchors.
{
  $B/tests/lob_io_tests
  $B/tests/lob_io_alloc_tests
  $B/tests/lob_engine_tests --gtest_filter='EngineDifferential.*'
} > "$E/io_tests.log" 2>&1 || die "io tests"
fi
[ "$FROM" -gt 1 ] && echo "resumed at stage $FROM at $(date -u +%Y-%m-%dT%H:%M:%SZ) (stages before it from the earlier run)" >> "$E/environment.txt"
rm -rf "$E/soak" && mkdir -p "$E/soak"

# 3. Process crashes: more kills, Release build.
bash tests/cli/crash_restart.sh $B/apps/lob_replay $B/testkit/lob_gen_commands /tmp/lob_crash_evidence 40 \
  > "$E/crash_restart.log" 2>&1 || die "crash_restart"
bash tests/cli/golden_cli.sh $B/apps/lob_replay tests/golden /tmp/lob_golden_evidence \
  > "$E/golden_cli.log" 2>&1 || die "golden_cli"

# 4. M4 long-run matrix (regression of the M4 capability).
start=$(date +%s)
bash scripts/soak.sh "$E/soak" > "$E/soak_summary.log" 2>&1
rc=$?
echo "soak.sh rc=$rc wall_seconds=$(( $(date +%s) - start ))" >> "$E/soak_summary.log"
[ $rc -eq 0 ] || die "soak.sh"

# 5. Mutation tests (M5 table, plus M2/M3 regression).
for m in m5_replay m3_engine m2_book; do
  python3 scripts/mutation/$m.py "$PWD" > "$E/mutations_$m.log" 2>&1
  grep -q "^\[BASELINE\] unmutated tree: build rc=0 ctest rc=0" "$E/mutations_$m.log" \
    && grep -q "^survivors: " "$E/mutations_$m.log" || die "incomplete mutation run $m"
done
echo done
