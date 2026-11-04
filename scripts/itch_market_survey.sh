#!/usr/bin/env bash
# Market-wide real-data validation (D-056), streaming, nothing stored but
# small text reports.
#
#   scripts/itch_market_survey.sh OUT_DIR [URL] [BENCH_BYTES]     (SKIP_BENCH=1: run 1 only)
#
# Run 1: one download split by tee into lob_itch_market single-threaded and
#        with 4 shards; per-instrument hash files compared with cmp.
# Run 2: a second download, the first BENCH_BYTES (default 3 GB) of the
#        decompressed stream held in memory, replayed single-threaded and
#        with N = 1, 2, 4 shards (throughput; summary hashes must agree).
# Needs a gcc-release build and network access to the source.
set -uo pipefail
cd "$(dirname "$0")/.."
out="${1:?OUT_DIR}"
url="${2:-https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/01302019.NASDAQ_ITCH50.gz}"
bench_bytes="${3:-3000000000}"
bin="${LOB_ITCH_MARKET:-build/gcc-release/apps/lob_itch_market}"
mkdir -p "${out}"
rm -f "${out}/single.fifo"; mkfifo "${out}/single.fifo"

start=$(date +%s.%N)
"${bin}" --input "${out}/single.fifo" --shards 0 --hashes "${out}/hashes_single.txt" > "${out}/single.out" 2>&1 &
p1=$!
curl -sS --fail -w '%{stderr}curl: http=%{http_code} bytes=%{size_download} time=%{time_total}s\n' "${url}" 2> "${out}/curl1.err" \
  | gzip -dc | tee "${out}/single.fifo" \
  | "${bin}" --input - --shards 4 --hashes "${out}/hashes_shards4.txt" > "${out}/shards4.out" 2>&1
st=("${PIPESTATUS[@]}")
wait "${p1}"; s1=$?
end=$(date +%s.%N)
rm -f "${out}/single.fifo"
{
  echo "run1 exit: curl=${st[0]} gzip=${st[1]} tee=${st[2]} shards4=${st[3]} single=${s1}"
  echo "run1 wall_s=$(echo "${end} - ${start}" | bc)"
  cat "${out}/curl1.err"
  if cmp -s "${out}/hashes_single.txt" "${out}/hashes_shards4.txt"; then echo "per-instrument hashes: IDENTICAL ($(wc -l < "${out}/hashes_single.txt") instruments)"; else echo "per-instrument hashes: DIFFER"; fi
  grep -h summary_hash "${out}/single.out" "${out}/shards4.out"
} | tee "${out}/status.txt"

[[ "${SKIP_BENCH:-0}" == "1" ]] && exit 0

# Run 2: throughput on an in-memory prefix (head closes the pipe early; curl's exit status is then 23).
curl -sS "${url}" 2> "${out}/curl2.err" | gzip -dc 2>/dev/null | head -c "${bench_bytes}" \
  | "${bin}" --input - --bench-bytes "${bench_bytes}" > "${out}/bench.out" 2>&1
echo "run2 exit: ${PIPESTATUS[*]} (curl 23 / gzip 141 expected: the prefix ends the pipe)" | tee -a "${out}/status.txt"
grep -E "bench:|frames_per_s|summary_hash" "${out}/bench.out" | tee -a "${out}/status.txt"
