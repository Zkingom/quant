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
docs/handbook/remeasure.sh --from-log LOG # import benchmarks run by hand (no build), then rebuild
```

Wall-clock numbers (throughput, latency, tick-to-trade) carry a † in the PDFs. `remeasure.sh` marks every number it
measures (or imports with `--from-log`) `measured_on: "local YYYY-MM-DD"`, and the page footer follows automatically:
"pending local re-measurement", "partly re-measured locally" or "measured locally". Entries marked `kept: true` are
measurements of a named machine kept for comparison (the cloud-VM tick-to-trade); they are never overwritten and the
footer ignores them. Current state: the L3′ / L4 synthetic workloads were re-measured on the author's WSL2 on
2026-11-05 (log in `MODULE_NOTES/evidence/local_wsl2_2026-11-05/`); the L1 engine, E1 and market-wide numbers keep
their original origin, listed in Appendix A.
Python packages are pinned in `requirements.txt` and installed into `docs/handbook/.venv` (git-ignored).
