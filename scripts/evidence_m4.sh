#!/usr/bin/env bash
# Regenerates MODULE_NOTES/evidence/M4/ from a clean build (run inside WSL/Linux).
# Usage: scripts/evidence_m4.sh
set -u
cd "$(dirname "$0")/.."
E=MODULE_NOTES/evidence/M4
rm -rf "$E"
mkdir -p "$E/soak"

{
  echo "# captured: $(date -u +%Y-%m-%dT%H:%M:%SZ)  base commit: $(git rev-parse --short HEAD) + M4 working tree (committed together with this file)"
  uname -srm; head -1 /etc/os-release; nproc | sed 's/^/cpus: /'
  grep -m1 "model name" /proc/cpuinfo; grep MemTotal /proc/meminfo
  g++ --version | head -1; clang++ --version | head -1; cmake --version | head -1; echo "ninja $(ninja --version)"
} > "$E/environment.txt"

# 1. Full regression, all six presets, from scratch.
rm -rf build
start=$(date +%s)
bash scripts/check.sh > "$E/check_all_presets.log" 2>&1
rc=$?
echo "check.sh rc=$rc wall_seconds=$(( $(date +%s) - start ))" >> "$E/check_all_presets.log"
[ $rc -eq 0 ] || { echo "check.sh FAILED"; exit 1; }

# 2. Regression anchors and contract tests, verbatim output.
{
  ./build/gcc-release/tests/lob_engine_tests --gtest_filter='EngineDifferential.*'
  ./build/gcc-release/tests/lob_book_tests --gtest_filter='OrderBookDifferential.*'
  ./build/gcc-release/tests/lob_engine_alloc_tests --gtest_filter='EngineAllocFailure.LongRuns*:ReplayContract.*'
} > "$E/anchors_and_contract.log" 2>&1

# 3. Leak-detection canary: proves LeakSanitizer is active in this
#    environment, so "no leak reported" in the ASan runs means something.
printf 'int main() { new int[16]; return 0; }\n' > /tmp/lsan_canary.cpp
g++ -fsanitize=address /tmp/lsan_canary.cpp -o /tmp/lsan_canary
{ ASAN_OPTIONS=detect_leaks=1 /tmp/lsan_canary; echo "canary exit=$?"; } > "$E/lsan_canary.log" 2>&1

# 4. Long-run matrix.
start=$(date +%s)
bash scripts/soak.sh "$E/soak" > "$E/soak_summary.log" 2>&1
rc=$?
echo "soak.sh rc=$rc wall_seconds=$(( $(date +%s) - start ))" >> "$E/soak_summary.log"
[ $rc -eq 0 ] || { echo "soak.sh FAILED"; exit 1; }

# 5. Mutation regression for M2 and M3 through the refactored test tree.
python3 scripts/mutation/m3_engine.py "$PWD" > "$E/mutations_m3.log" 2>&1
python3 scripts/mutation/m2_book.py "$PWD" > "$E/mutations_m2.log" 2>&1
# A mutation run that did not complete must not look like a pass.
for f in "$E/mutations_m3.log" "$E/mutations_m2.log"; do
  grep -q "^\[BASELINE\] unmutated tree: build rc=0 ctest rc=0" "$f" && grep -q "^survivors: " "$f"     || { echo "INCOMPLETE MUTATION RUN: $f"; exit 1; }
done
echo done
