#!/usr/bin/env bash
# Long-run matrix (run inside WSL/Linux after building the presets used):
#   scripts/soak.sh OUTDIR
# Writes one log per (build, profile, mode) to OUTDIR. Each profile runs in
# its own process so the reported max RSS belongs to that profile.
# Every failure prints a [repro] line; any failure or unmet target makes the
# script exit non-zero.
set -u
cd "$(dirname "$0")/.."
OUT=${1:?usage: scripts/soak.sh OUTDIR}
mkdir -p "$OUT"
PROFILES=$(./build/gcc-release/testkit/lob_soak --list | awk '{print $1}')
status=0

run() {  # run BUILD PROFILE MODE ARGS...
  local build=$1 profile=$2 mode=$3
  shift 3
  local log="$OUT/${build}.${profile}.${mode}.log"
  echo "\$ lob_soak --profile $profile $*" > "$log"
  if ! ./build/$build/testkit/lob_soak --profile "$profile" "$@" >> "$log" 2>&1; then
    echo "FAILED: $log"; status=1
  fi
  grep -E "^\[time\]|^\[resources\]|^\[targets\]|^\[failure\]|^\[repro\]" "$log" | sed "s|^|$build $profile $mode: |"
}

for p in $PROFILES; do
  # Long runs: 10 seeds x 200k commands, targets enforced.
  run gcc-release "$p" long --seeds 1-10 --commands 200000 --check-every 1000 --require-targets
  # Repeated allocation failures (2% of commands armed), run continues.
  run gcc-release "$p" faults --seeds 11-15 --commands 50000 --check-every 500 --fault-per-mille 20 --require-targets
  # Cross-compiler determinism: same arguments under Clang.
  run gcc-release "$p" xcheck --seeds 21-23 --commands 50000 --check-every 1000
  run clang-release "$p" xcheck --seeds 21-23 --commands 50000 --check-every 1000
  # Sanitizers (ASan+UBSan+LSan) with faults, shorter.
  run gcc-asan "$p" asan --seeds 31-32 --commands 20000 --check-every 200 --fault-per-mille 20
done

# GCC vs Clang: the coverage/state/fault lines must be identical.
for p in $PROFILES; do
  if ! diff <(grep -E "^\[(coverage|state|faults)\]" "$OUT/gcc-release.$p.xcheck.log") \
            <(grep -E "^\[(coverage|state|faults)\]" "$OUT/clang-release.$p.xcheck.log") > /dev/null; then
    echo "GCC/Clang DIFFER: $p"; status=1
  else
    echo "gcc/clang identical: $p"
  fi
done
exit $status
