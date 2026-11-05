#!/usr/bin/env bash
# Build and run the libFuzzer target for the OUCH 4.2 codec (L4.1).
# Usage: scripts/fuzz_ouch.sh [SECONDS] [BUILD_DIR]
# Requires Clang with the fuzzer runtime (Ubuntu: libclang-rt-18-dev).
# The seed corpus is the hand-written samples (tests/golden/ouch/samples.txt).
set -euo pipefail

cd "$(dirname "$0")/.."
secs="${1:-60}"
build="${2:-build/fuzz}"

cmake -S . -B "${build}" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ \
      -DLOB_FUZZ=ON -DLOB_BUILD_EXAMPLES=OFF >/dev/null
cmake --build "${build}" --target lob_fuzz_ouch

corpus="${build}/corpus_ouch"
mkdir -p "${corpus}"
python3 - "${corpus}" <<'PY'
import sys
for line in open('tests/golden/ouch/samples.txt'):
    if not line.strip() or line.startswith('#'):
        continue
    direction, name, *hexparts = line.split()
    open(f"{sys.argv[1]}/{direction}_{name}.bin", 'wb').write(bytes.fromhex(''.join(hexparts)))
PY
"${build}/tests/lob_fuzz_ouch" -max_total_time="${secs}" -print_final_stats=1 "${corpus}"
