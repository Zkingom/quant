#!/usr/bin/env bash
# Build and run the libFuzzer target for the ITCH/feed path (L3' 1e).
# Usage: scripts/fuzz_itch.sh [SECONDS] [BUILD_DIR]
# Requires Clang with the fuzzer runtime (Ubuntu: libclang-rt-18-dev).
# The seed corpus is the hand-built sample session (tests/golden/itch).
set -euo pipefail

cd "$(dirname "$0")/.."
secs="${1:-60}"
build="${2:-build/fuzz}"

cmake -S . -B "${build}" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ \
      -DLOB_FUZZ=ON -DLOB_BUILD_EXAMPLES=OFF >/dev/null
cmake --build "${build}" --target lob_fuzz_itch_feed

corpus="${build}/corpus"
mkdir -p "${corpus}"
python3 - "${corpus}/session.bin" <<'PY'
import sys
data = bytes.fromhex(''.join(open('tests/golden/itch/session.hex').read().split()))
open(sys.argv[1], 'wb').write(data)
PY
"${build}/tests/lob_fuzz_itch_feed" -max_total_time="${secs}" -print_final_stats=1 "${corpus}"
