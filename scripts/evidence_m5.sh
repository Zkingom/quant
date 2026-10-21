#!/usr/bin/env bash
# Regenerates MODULE_NOTES/evidence/M5/ from a clean build (run inside WSL/Linux).
# Every stage must succeed; a failing stage makes the script exit non-zero.
#
# Usage:
#   scripts/evidence_m5.sh            fresh run (deletes the evidence directory)
#   RESUME=1 scripts/evidence_m5.sh   continue an interrupted run: stages that
#                                     completed are skipped, but ONLY if the
#                                     source tree is byte-identical to the one
#                                     the run started with (hash recorded at
#                                     the start); otherwise it refuses.
set -u
cd "$(dirname "$0")/.."
E=MODULE_NOTES/evidence/M5
B=$PWD/build/gcc-release   # absolute: the CLI scripts change directory
die() { echo "EVIDENCE FAILED: $*"; exit 1; }

tree_hash() {
  git ls-files -co --exclude-standard -- . ':!MODULE_NOTES/evidence' | LC_ALL=C sort \
    | xargs -d '\n' sha1sum | sha1sum | cut -c1-16
}

if [ "${RESUME:-0}" = "1" ]; then
  [ -f "$E/tree_hash" ] || die "nothing to resume"
  [ "$(cat "$E/tree_hash")" = "$(tree_hash)" ] || die "source tree changed since the run started; run fresh"
  echo "# resumed: $(date -u +%Y-%m-%dT%H:%M:%SZ) (tree hash unchanged: $(cat "$E/tree_hash"))" >> "$E/environment.txt"
else
  rm -rf "$E"
  mkdir -p "$E"
  tree_hash > "$E/tree_hash"
  {
    echo "# captured: $(date -u +%Y-%m-%dT%H:%M:%SZ)  base commit: $(git rev-parse --short HEAD) (+ working tree, tree hash $(cat "$E/tree_hash"))"
    uname -srm; head -1 /etc/os-release; nproc | sed 's/^/cpus: /'
    grep -m1 "model name" /proc/cpuinfo; grep MemTotal /proc/meminfo
    df -T /tmp | tail -1 | awk '{print "/tmp filesystem: " $2}'
    df -T . | tail -1 | awk '{print "source tree filesystem: " $2}'
    g++ --version | head -1; clang++ --version | head -1; cmake --version | head -1; echo "ninja $(ninja --version)"
  } > "$E/environment.txt"
fi

stage() {  # stage NAME: returns 0 if it still has to run
  [ ! -f "$E/.done.$1" ]
}
done_stage() { date -u +%Y-%m-%dT%H:%M:%SZ > "$E/.done.$1"; }

# 1. Full regression, all six presets, from scratch (includes CLI golden and crash tests).
if stage check; then
  rm -rf build
  start=$(date +%s)
  bash scripts/check.sh > "$E/check_all_presets.log" 2>&1
  rc=$?
  echo "check.sh rc=$rc wall_seconds=$(( $(date +%s) - start ))" >> "$E/check_all_presets.log"
  [ $rc -eq 0 ] || die "check.sh"
  done_stage check
fi

# 2. Verbatim outputs of the M5 acceptance tests and the M4 anchors.
if stage io; then
  {
    "$B/tests/lob_io_tests"
    "$B/tests/lob_io_alloc_tests"
    "$B/tests/lob_engine_tests" --gtest_filter='EngineDifferential.*'
  } > "$E/io_tests.log" 2>&1 || die "io tests"
  done_stage io
fi

# 3. Process crashes (more kills, Release build) and the CLI golden test.
if stage cli; then
  bash tests/cli/crash_restart.sh "$B/apps/lob_replay" "$B/testkit/lob_gen_commands" /tmp/lob_crash_evidence 40 \
    > "$E/crash_restart.log" 2>&1 || die "crash_restart"
  bash tests/cli/golden_cli.sh "$B/apps/lob_replay" tests/golden /tmp/lob_golden_evidence \
    > "$E/golden_cli.log" 2>&1 || die "golden_cli"
  done_stage cli
fi

# 4. M4 long-run matrix (regression of the M4 capability).
if stage soak; then
  rm -rf "$E/soak" && mkdir -p "$E/soak"
  start=$(date +%s)
  bash scripts/soak.sh "$E/soak" > "$E/soak_summary.log" 2>&1
  rc=$?
  echo "soak.sh rc=$rc wall_seconds=$(( $(date +%s) - start ))" >> "$E/soak_summary.log"
  [ $rc -eq 0 ] || die "soak.sh"
  done_stage soak
fi

# 5. Mutation tests (M5 table, plus M2/M3 regression), one stage per table.
for m in m5_replay m3_engine m2_book; do
  if stage "$m"; then
    python3 scripts/mutation/$m.py "$PWD" > "$E/mutations_$m.log" 2>&1
    grep -q "^\[BASELINE\] unmutated tree: build rc=0 ctest rc=0" "$E/mutations_$m.log" \
      && grep -q "^survivors: " "$E/mutations_$m.log" || die "incomplete mutation run $m"
    done_stage "$m"
  fi
done
echo done
