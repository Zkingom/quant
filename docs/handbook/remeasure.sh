#!/usr/bin/env bash
# Re-measure the handbooks' wall-clock numbers on THIS machine, write them into
# docs/handbook/numbers.yaml and regenerate both PDFs.
#
#   docs/handbook/remeasure.sh [--itch FILE] [--l2]
#   docs/handbook/remeasure.sh --from-log LOG
#
#   (default)    lob_bench (engine, 7 profiles x 5 reps), lob_itch_bench
#                (1,000,000 messages x 5 reps), lob_t2t (3 runs sleeping +
#                3 runs busy-polling), pinned to cores when there are >= 4
#   --itch FILE  also market-wide throughput on a real (decompressed) ITCH file
#   --l2         also re-run scripts/l2_reproduce.sh (about 73 minutes) for E1
#   --from-log LOG  measure nothing: import a log of the lob_itch_bench / lob_t2t
#                commands run by hand (BENCHMARK §13 / §15), then rebuild
#
# Every number it updates gets measured_on "local YYYY-MM-DD"; the footer of
# the handbooks then says so. Numbers it cannot re-measure keep their origin.
# Takes about 10-15 minutes without --l2. Run it on a quiet machine.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "${here}/../.." && pwd)"
itch=""
l2=0
log=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --itch) itch="$2"; shift 2 ;;
    --l2) l2=1; shift ;;
    --from-log) log="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"; shift 2 ;;
    *) echo "remeasure.sh: unknown option $1" >&2; exit 2 ;;
  esac
done

cd "${root}"
if [[ -n "${log}" ]]; then
  args=(--from-log "${log}")
else
  cmake --preset gcc-release >/dev/null
  cmake --build --preset gcc-release --target lob_bench lob_itch_bench lob_t2t lob_itch_market
  args=(--bin "${root}/build/gcc-release")
  [[ -n "${itch}" ]] && args+=(--itch "${itch}")
fi
if [[ ${l2} -eq 1 ]]; then
  scripts/l2_reproduce.sh
  args+=(--l2-summary "${root}/MODULE_NOTES/evidence/L2/reproduce/summary.md")
fi

cd "${here}"
if [[ ! -x .venv/bin/python ]] || ! cmp -s requirements.txt .venv/requirements.txt; then
  python3 -m venv .venv
  .venv/bin/pip install --quiet --disable-pip-version-check -r requirements.txt
  cp requirements.txt .venv/requirements.txt
fi
.venv/bin/python remeasure.py "${args[@]}"
./build.sh
