#!/usr/bin/env bash
# Real-data validation of the ITCH path (L3', D-054), streaming, NOTHING
# written to disk except small text reports.
#
#   scripts/itch_real_survey.sh SYMBOL OUT_DIR [URL]
#
# One download is split with tee into three consumers of the same bytes:
#   lob_itch_survey SYMBOL                       -> OUT_DIR/survey.out
#   lob_itch --mode pipeline (decoder thread -> SPSC -> book thread) -> OUT_DIR/pipeline.out
#   lob_itch --mode single                        -> OUT_DIR/single.out
# and finally compares the two modes' event_hash / book_hash.
# Needs a gcc-release build (cmake --preset gcc-release && cmake --build --preset gcc-release)
# and network access to the source (default: the Nasdaq sample used by scripts/fetch_itch.sh).
# Memory: each consumer is bounded by its own configuration (~0.1 GB each); the
# compressed file is ~4.8 GB, decompressed ~11 GB -- neither is stored.
set -uo pipefail
cd "$(dirname "$0")/.."

sym="${1:?SYMBOL}"
out="${2:?OUT_DIR}"
url="${3:-https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/01302019.NASDAQ_ITCH50.gz}"
bin=build/gcc-release
mkdir -p "${out}"
rm -f "${out}/pipe.fifo" "${out}/single.fifo"
mkfifo "${out}/pipe.fifo" "${out}/single.fifo"

start=$(date +%s.%N)
"${bin}/apps/lob_itch" --input "${out}/pipe.fifo" --symbol "${sym}" --mode pipeline --stride 400 > "${out}/pipeline.out" 2>&1 &
p1=$!
"${bin}/apps/lob_itch" --input "${out}/single.fifo" --symbol "${sym}" --mode single --samples 0 > "${out}/single.out" 2>&1 &
p2=$!
curl -sS --fail -w '%{stderr}curl: http=%{http_code} bytes=%{size_download} time=%{time_total}s speed=%{speed_download}B/s\n' \
     "${url}" 2> "${out}/curl.err" \
  | gzip -dc 2> "${out}/gzip.err" \
  | tee "${out}/pipe.fifo" "${out}/single.fifo" \
  | "${bin}/bench/lob_itch_survey" --symbol "${sym}" --context 12 --max-points 120 > "${out}/survey.out" 2> "${out}/survey.err"
st=("${PIPESTATUS[@]}")
wait "${p1}"; s1=$?
wait "${p2}"; s2=$?
end=$(date +%s.%N)
rm -f "${out}/pipe.fifo" "${out}/single.fifo"

{
  echo "exit: curl=${st[0]} gzip=${st[1]} tee=${st[2]} survey=${st[3]} lob_itch_pipeline=${s1} lob_itch_single=${s2}"
  echo "total_wall_s=$(echo "${end} - ${start}" | bc)"
  cat "${out}/curl.err"
  hp=$(grep -o 'event_hash=0x[0-9a-f]* book_hash=0x[0-9a-f]*' "${out}/pipeline.out")
  hs=$(grep -o 'event_hash=0x[0-9a-f]* book_hash=0x[0-9a-f]*' "${out}/single.out")
  echo "pipeline: ${hp}"
  echo "single:   ${hs}"
  if [[ -n "${hp}" && "${hp}" == "${hs}" ]]; then echo "modes: IDENTICAL"; else echo "modes: DIFFER"; fi
} | tee "${out}/status.txt"
