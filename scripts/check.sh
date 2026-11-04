#!/usr/bin/env bash
# Configure, build and test one or more CMake presets.
# Usage: scripts/check.sh [--tsan] [preset...]
#   default: the six presets (GCC/Clang x Debug/Release/ASan+UBSan)
#   --tsan:  additionally gcc-tsan and clang-tsan, which build everything but
#            run only tests labelled "concurrency" (L3', D-047)
set -euo pipefail

cd "$(dirname "$0")/.."

tsan=0
presets=()
for arg in "$@"; do
  if [[ "${arg}" == "--tsan" ]]; then
    tsan=1
  else
    presets+=("${arg}")
  fi
done
if [[ ${#presets[@]} -eq 0 ]]; then
  presets=(gcc-debug gcc-release gcc-asan clang-debug clang-release clang-asan)
fi
if [[ ${tsan} -eq 1 ]]; then
  presets+=(gcc-tsan clang-tsan)
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
