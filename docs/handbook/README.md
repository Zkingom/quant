# 复习手册 / Review handbooks

Two printable PDFs that cover the whole project (L1, L2, L3′, L4; L5 and L6 as future plans), with the same outline and section numbers:

- [handbook-zh.pdf](handbook-zh.pdf) — 中文版
- [handbook-en.pdf](handbook-en.pdf) — English edition (written to be said out loud in interviews, not a translation)

| File | Role |
|---|---|
| `content/zh.md`, `content/en.md` | the text; the build fails if their numbered headings differ |
| `numbers.yaml` | every number in the handbooks, with its source file; `{{n:key}}` in the text |
| `diagrams.py` | the five figures as SVG (black and white: line styles and hatching, no colour) |
| `style.css` | A4 print layout: binding margin alternates for duplex, body 10.5 pt |
| `build.py`, `build.sh` | build both PDFs; checks outline parity, unresolved placeholders, glyph coverage (no tofu), font embedding; renders every page to `_render/` |
| `remeasure.py`, `remeasure.sh` | re-measure the wall-clock numbers on this machine, update `numbers.yaml`, rebuild |

## Commands

```bash
sudo apt install python3-venv fonts-noto-cjk fonts-dejavu-core poppler-utils   # once
docs/handbook/build.sh                    # rebuild both PDFs
docs/handbook/remeasure.sh                # re-measure wall-clock numbers locally, then rebuild
docs/handbook/remeasure.sh --itch FILE    # + market-wide throughput on a real ITCH file
docs/handbook/remeasure.sh --l2           # + the L2 baseline comparison (about 73 minutes)
```

Wall-clock numbers (throughput, latency, tick-to-trade) carry a † in the PDFs. Until they are re-measured on the
author's machine the page footer says "pending local re-measurement"; `remeasure.sh` marks every number it measures
`measured_on: "local YYYY-MM-DD"`, and the footer follows automatically (partly / fully measured locally).
Python packages are pinned in `requirements.txt` and installed into `docs/handbook/.venv` (git-ignored).
