#!/usr/bin/env bash
# Regenerates MODULE_NOTES/evidence/M3/ from a clean build (run inside WSL/Linux).
# Usage: scripts/evidence_m3.sh
set -u
cd "$(dirname "$0")/.."
E=MODULE_NOTES/evidence/M3
mkdir -p "$E"

{
  echo "# captured: $(date -u +%Y-%m-%dT%H:%M:%SZ)  base commit: $(git rev-parse --short HEAD) + M3 working tree (committed together with this file)"
  uname -srm; head -1 /etc/os-release
  g++ --version | head -1; clang++ --version | head -1; cmake --version | head -1; echo "ninja $(ninja --version)"
} > "$E/environment.txt"

rm -rf build
bash scripts/check.sh > "$E/check_all_presets.log" 2>&1
echo "check.sh rc=$?" >> "$E/check_all_presets.log"

for p in gcc-debug gcc-asan clang-release; do
  echo "== $p =="
  ./build/$p/tests/lob_engine_tests --gtest_filter='*Differential*' | grep -E 'coverage|OK \]|FAILED'
  ./build/$p/tests/lob_engine_alloc_tests | grep -E 'alloc\]|PASSED|FAILED'
done > "$E/differential_and_alloc.log" 2>&1

./build/gcc-release/examples/engine_demo > "$E/engine_demo.out" 2>&1
python3 scripts/mutation/m3_engine.py "$PWD" > "$E/mutations_m3.log" 2>&1
python3 scripts/mutation/m2_book.py "$PWD" > "$E/mutations_m2_regression.log" 2>&1
echo done
