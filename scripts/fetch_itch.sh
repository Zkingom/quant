#!/usr/bin/env bash
# Download a Nasdaq TotalView-ITCH 5.0 sample day for LOCAL use (L3' 1e).
# Real ITCH data never enters the repository: data/ is in .gitignore.
#
# Usage: scripts/fetch_itch.sh [URL] [DEST_DIR]
#   URL      default: the 2019-01-30 sample from Nasdaq's public EMI site
#   DEST_DIR default: data/itch
#
# Memory: curl streams to disk (constant memory); the compressed file is
# several GB, so check free DISK space, not RAM. The file is kept
# compressed; replay decompresses on the fly through a pipe, so neither
# step loads the file into memory (README "L3' 行情回放"):
#   gzip -dc data/itch/01302019.NASDAQ_ITCH50.gz | \
#     build/gcc-release/apps/lob_itch --input - --symbol AAPL
# The download resumes (-C -) if interrupted; `gzip -t` verifies it in a
# streaming pass.
set -euo pipefail

cd "$(dirname "$0")/.."

url="${1:-https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/01302019.NASDAQ_ITCH50.gz}"
dest="${2:-data/itch}"
mkdir -p "${dest}"
file="${dest}/$(basename "${url//%20/ }")"
file="${file// /_}"

echo "downloading ${url}"
echo "        to ${file}"
curl --fail --location --retry 5 --retry-delay 5 -C - -o "${file}" "${url}"

echo "verifying gzip stream (streaming, constant memory)"
gzip -t "${file}"
ls -l "${file}"
echo "ok"
