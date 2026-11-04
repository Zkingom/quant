#!/usr/bin/env bash
# CI performance-regression gate on INSTRUCTION COUNTS (L3' stage 4, D-051).
#
#   scripts/icount_gate.sh            build, measure, compare with bench/icount_baseline.txt
#   scripts/icount_gate.sh --update   build, measure, rewrite the baseline (see below)
#
# Why instructions and not wall-clock time: on shared CI runners the clock
# varies by tens of percent between runs; cachegrind's instruction count
# (Ir) of a fixed single-threaded workload is exactly reproducible for a
# given binary. It is a proxy -- it ignores cache misses, branch mispredicts
# and the memory system -- so it catches "more work" regressions, not every
# slowdown. Wall-clock benchmarks stay local (BENCHMARK §13).
#
# The baseline is only comparable with the SAME compiler, valgrind and libc:
# the script refuses (exit 3) when `g++ --version`, `valgrind --version` or
# `ldd --version` differs from the baseline header, instead of reporting a
# false regression. CI pins the runner image (ubuntu-24.04: GCC 13.3,
# valgrind 3.22, glibc 2.39). Residual variation: glibc picks memcpy
# variants by CPU (ifunc), so runners with different CPUs can differ
# slightly; the tolerance absorbs this (measured: see MODULE_NOTES/L3).
#
# Gate: fail (exit 1) if any scenario's Ir exceeds the baseline by more than
# ICOUNT_TOLERANCE_PCT (default 2). An improvement of more than the
# tolerance is reported (exit 0) with a reminder to update the baseline, so
# that gains are locked in.
#
# Baseline update procedure (who / when / how):
#   when: a change intentionally alters the measured code paths' work
#         (feature, accepted optimisation, or an expected regression), or the
#         CI image / compiler / valgrind changes;
#   who:  the author of that change, in the same pull request; the reviewer
#         approves the new numbers like any other change;
#   how:  on the pinned toolchain (the CI image, or the same compiler and
#         valgrind versions locally) run `scripts/icount_gate.sh --update`,
#         commit bench/icount_baseline.txt with the old -> new numbers and
#         the reason in the commit message. Never update the baseline only
#         to make a red gate green without an explanation.
set -euo pipefail

cd "$(dirname "$0")/.."
update=0
[[ "${1:-}" == "--update" ]] && update=1
tol="${ICOUNT_TOLERANCE_PCT:-2}"
build="${ICOUNT_BUILD_DIR:-build/icount}"
baseline="bench/icount_baseline.txt"

if ! log="$(cmake -S . -B "${build}" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++ \
                 -DLOB_BUILD_EXAMPLES=OFF 2>&1 && cmake --build "${build}" --target lob_itch_icount 2>&1)"; then
  echo "${log}" | tail -40
  echo "build failed"
  exit 2
fi

compiler="$(g++ --version | head -1)"
vg="$(valgrind --version) / $(ldd --version 2>/dev/null | sed -n 1p)"  # libc: memcpy/memmove are in the measured region
declare -A ir digest
for s in decode book trading; do
  out="${build}/cachegrind.${s}.out"
  line="$(valgrind --tool=cachegrind --cache-sim=no --instr-at-start=no \
            --cachegrind-out-file="${out}" "${build}/bench/lob_itch_icount" "${s}" 2>/dev/null)"
  digest[$s]="${line##*digest=}"
  ir[$s]="$(awk '/^summary:/ {print $2}' "${out}")"
  echo "${s}: Ir=${ir[$s]} digest=${digest[$s]}"
done

if [[ ${update} -eq 1 ]]; then
  {
    echo "# Instruction-count baseline for scripts/icount_gate.sh (see its header)."
    echo "# compiler: ${compiler}"
    echo "# valgrind: ${vg}"
    for s in decode book trading; do echo "${s} ${ir[$s]} ${digest[$s]}"; done
  } > "${baseline}"
  echo "baseline written to ${baseline}"
  exit 0
fi

want_cc="$(sed -n 's/^# compiler: //p' "${baseline}")"
want_vg="$(sed -n 's/^# valgrind: //p' "${baseline}")"
if [[ "${want_cc}" != "${compiler}" || "${want_vg}" != "${vg}" ]]; then
  echo "toolchain differs from the baseline:"
  echo "  baseline: ${want_cc} / ${want_vg}"
  echo "  here:     ${compiler} / ${vg}"
  echo "instruction counts are not comparable; run on the pinned toolchain or update the baseline (see header)"
  exit 3
fi

status=0
printf '| scenario | baseline Ir | current Ir | change |\n|---|---|---|---|\n'
while read -r s base base_digest; do
  [[ -z "${s}" || "${s}" == \#* ]] && continue
  cur="${ir[$s]}"
  pct="$(awk -v c="${cur}" -v b="${base}" 'BEGIN { printf "%+.3f", (c - b) * 100.0 / b }')"
  printf '| %s | %s | %s | %s%% |\n' "${s}" "${base}" "${cur}" "${pct}"
  if [[ "${digest[$s]}" != "${base_digest}" ]]; then
    echo "  ${s}: workload digest changed (${base_digest} -> ${digest[$s]}): the workload is not the same; update the baseline"
    status=1
  fi
  if awk -v p="${pct}" -v t="${tol}" 'BEGIN { exit !(p > t) }'; then
    echo "  ${s}: REGRESSION above ${tol}%"
    status=1
  elif awk -v p="${pct}" -v t="${tol}" 'BEGIN { exit !(p < -t) }'; then
    echo "  ${s}: improved by more than ${tol}%: consider updating the baseline to lock it in"
  fi
done < "${baseline}"
exit ${status}
