#!/usr/bin/env bash
# Build the two printable review handbooks: docs/handbook/handbook-zh.pdf, handbook-en.pdf.
#
#   docs/handbook/build.sh
#
# Needs: python3 (venv), the fonts below, poppler-utils (pdffonts / pdfinfo / pdftoppm).
#   sudo apt install python3-venv fonts-noto-cjk fonts-dejavu-core poppler-utils
# Python packages are pinned in requirements.txt and installed into docs/handbook/.venv.
# The build fails if the two editions' outlines differ, a placeholder does not
# resolve, a character has no glyph in the fonts, or a font is not embedded.
# Page images for a visual check: docs/handbook/_render/.
set -euo pipefail

cd "$(dirname "$0")"
for tool in pdffonts pdfinfo pdftoppm fc-match; do
  command -v "${tool}" >/dev/null || { echo "build.sh: ${tool} missing (sudo apt install poppler-utils fontconfig)" >&2; exit 1; }
done
if [[ ! -x .venv/bin/python ]] || ! cmp -s requirements.txt .venv/requirements.txt; then
  python3 -m venv .venv
  .venv/bin/pip install --quiet --disable-pip-version-check -r requirements.txt
  cp requirements.txt .venv/requirements.txt
fi
.venv/bin/python build.py
