#!/usr/bin/env bash
# Configure, build and test one or more CMake presets.
# Usage: scripts/check.sh [preset...]   (default: all six presets)
set -euo pipefail

cd "$(dirname "$0")/.."

presets=("$@")
if [[ ${#presets[@]} -eq 0 ]]; then
  presets=(gcc-debug gcc-release gcc-asan clang-debug clang-release clang-asan)
fi

failed=()
for preset in "${presets[@]}"; do
  echo "================ ${preset} ================"
  if cmake --preset "${preset}" >/dev/null &&
     cmake --build --preset "${preset}" &&
     ctest --preset "${preset}"; then
    echo "[PASS] ${preset}"
  else
    echo "[FAIL] ${preset}"
    failed+=("${preset}")
  fi
done

echo "================ summary ================"
if [[ ${#failed[@]} -eq 0 ]]; then
  echo "all presets passed: ${presets[*]}"
else
  echo "failed presets: ${failed[*]}"
  exit 1
fi
