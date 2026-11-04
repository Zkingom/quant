#!/usr/bin/env python3
"""Generate the README charts (SVG) from the committed CSVs in docs/charts/data.

    python3 scripts/charts/make_charts.py              # writes docs/charts/*.svg
    python3 scripts/charts/make_charts.py --png DIR    # also PNG previews into DIR (not committed)

Data provenance (source command, commit, environment) is in the header of
each CSV and printed under each figure. Requires matplotlib (pip install
matplotlib). Palette: the dataviz reference categorical slots 1-3, validated
for the light surface; values are direct-labelled (slot 3 is below 3:1
contrast, so labels are mandatory)."""

import csv
import os
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DATA = os.path.join(ROOT, "docs", "charts", "data")
OUT = os.path.join(ROOT, "docs", "charts")

SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK2 = "#52514e"
GRID = "#e4e3df"
S1, S2, S3 = "#2a78d6", "#eb6834", "#1baf7a"  # blue, orange, aqua
NEUTRAL = "#b9b8b2"

plt.rcParams.update({
    "font.family": "DejaVu Sans", "font.size": 9, "text.color": INK, "axes.labelcolor": INK2,
    "axes.edgecolor": GRID, "xtick.color": INK2, "ytick.color": INK2, "axes.facecolor": SURFACE,
    "figure.facecolor": SURFACE, "savefig.facecolor": SURFACE, "axes.grid": True, "grid.color": GRID,
    "grid.linewidth": 0.6, "axes.spines.top": False, "axes.spines.right": False, "svg.fonttype": "none",
})


def read(name):
    with open(os.path.join(DATA, name)) as f:
        lines = [l for l in f if not l.startswith("#")]
        notes = []
    with open(os.path.join(DATA, name)) as f:
        notes = [l[1:].strip() for l in f if l.startswith("#")]
    return list(csv.DictReader(lines)), " ".join(notes)


PNG_DIR = sys.argv[sys.argv.index("--png") + 1] if "--png" in sys.argv else None


def save(fig, name):
    fig.savefig(os.path.join(OUT, name + ".svg"))
    if PNG_DIR:
        fig.savefig(os.path.join(PNG_DIR, name + ".png"), dpi=110)
    plt.close(fig)


def caption(fig, text):
    fig.text(0.01, 0.005, text, fontsize=7, color=INK2, ha="left", va="bottom", wrap=True)


def latency():
    rows, notes = read("latency_waterfall.csv")
    labels = [f"{r['mode']} · {r['hop']}" for r in rows]
    fig, ax = plt.subplots(figsize=(8.6, 4.6))
    h = 0.26
    for i, (key, color, name) in enumerate([("p50", S1, "p50"), ("p99", S2, "p99"), ("p999", S3, "p99.9")]):
        ys = [j + (i - 1) * h for j in range(len(rows))]
        vals = [max(float(r[key]), 1.0) for r in rows]
        ax.barh(ys, vals, height=h - 0.03, color=color, label=name, zorder=3)
        for y, v in zip(ys, vals):
            ax.text(v * 1.08, y, f"{v:,.0f}", va="center", fontsize=6.5, color=INK2)
    ax.set_xscale("log")
    ax.set_yticks(range(len(rows)))
    ax.set_yticklabels(labels)
    ax.invert_yaxis()
    ax.set_xlabel("latency per hop, ns (log scale; timer overhead subtracted)")
    ax.set_title("Market-data pipeline latency waterfall (synthetic ITCH, 1 M messages)", loc="left", fontsize=10)
    ax.legend(loc="lower right", frameon=False, ncol=3)
    ax.grid(axis="y", visible=False)
    fig.subplots_adjust(left=0.30, right=0.97, top=0.92, bottom=0.17)
    caption(fig, notes)
    save(fig, "latency_waterfall")


def throughput():
    rows, notes = read("market_throughput.csv")
    fig, ax = plt.subplots(figsize=(7.2, 3.6))
    xs = range(len(rows))
    vals = [float(r["frames_per_s"]) / 1e6 for r in rows]
    ax.bar(xs, vals, width=0.55, color=S1, zorder=3)
    for x, v, r in zip(xs, vals, rows):
        ax.text(x, v + 0.05, f"{v:.2f} M/s\n({float(r['elapsed_s']):.1f} s)", ha="center", va="bottom", fontsize=8)
    ax.set_xticks(list(xs))
    ax.set_xticklabels([r["mode"] for r in rows])
    ax.set_ylabel("frames per second (millions)")
    ax.set_ylim(0, max(vals) * 1.3)
    ax.set_title("Market-wide book reconstruction: 8 713 instruments, real data (3 GB prefix)", loc="left", fontsize=10)
    ax.grid(axis="x", visible=False)
    fig.subplots_adjust(left=0.10, right=0.98, top=0.90, bottom=0.25)
    caption(fig, notes)
    save(fig, "market_throughput")


def sxtc():
    path = os.path.join(DATA, "sxtc_series.csv")
    if not os.path.exists(path):
        print("sxtc_series.csv missing; skipping the SXTC chart", file=sys.stderr)
        return
    rows, notes = read("sxtc_series.csv")
    # Split into pause episodes: an episode starts at an "H P" row.
    episodes, cur = [], None
    for r in rows:
        if r["event"].startswith("H P"):
            cur = []
            episodes.append(cur)
        if cur is not None:
            cur.append(r)
    fig, axes = plt.subplots(len(episodes), 2, figsize=(9.2, 3.2 * len(episodes)), squeeze=False,
                             gridspec_kw={"width_ratios": [1.15, 1]})
    for k, ep in enumerate(episodes):
        t_pause = int(ep[0]["ts_ns"])
        t_resume = next(int(r["ts_ns"]) for r in ep if r["event"].startswith("H T"))
        t_clean = next((int(r["ts_ns"]) for r in ep if int(r["ts_ns"]) >= t_resume and r["cross"] == "-"
                        and r["event"] not in ("H T",)), t_resume)
        for col, (lo, hi, scale, unit) in enumerate([(t_pause - 20e9, t_resume + 20e9, 60e9, "min"),
                                                     (t_resume - 400e3, t_resume + 400e3, 1e3, "µs")]):
            ax = axes[k][col]
            ref = t_pause if col == 0 else t_resume
            before = [r for r in ep if int(r["ts_ns"]) < lo]
            sel = ([dict(before[-1], ts_ns=str(int(lo)))] if before else []) + \
                  [r for r in ep if lo <= int(r["ts_ns"]) <= hi]
            sel.append(dict(sel[-1], ts_ns=str(int(hi))))  # hold the last state to the window's edge
            xs = [(int(r["ts_ns"]) - ref) / scale for r in sel]
            bid = [int(r["bid"]) / 1e4 if int(r["bid"]) else None for r in sel]
            ask = [int(r["ask"]) / 1e4 if int(r["ask"]) else None for r in sel]
            ax.step(xs, bid, where="post", color=S1, lw=2, label="best bid", zorder=3)
            ax.step(xs, ask, where="post", color=S2, lw=2, label="best ask", zorder=3)
            start = None  # crossed / locked stretches, shaded neutral
            for x, r in zip(xs, sel):
                if r["cross"] != "-" and start is None:
                    start = x
                if r["cross"] == "-" and start is not None:
                    ax.axvspan(start, x, color=NEUTRAL, alpha=0.35, lw=0, zorder=1)
                    start = None
            if start is not None:
                ax.axvspan(start, xs[-1], color=NEUTRAL, alpha=0.35, lw=0, zorder=1)
            ax.set_xlim(xs[0], xs[-1])
            ymin, ymax = ax.get_ylim()
            marks = [(t_pause, "H 'P' pause", "left"), (t_resume, "Q cross + H 'T'", "right")] if col == 0 else \
                    [(t_resume, "Q cross + H 'T'", "right"), (t_clean, "cross cleared", "left")]
            for t, label, side in marks:
                x = (t - ref) / scale
                ax.axvline(x, color=INK2, lw=0.8, ls="--", zorder=2)
                ax.text(x, ymax, (" " + label) if side == "left" else (label + " "), fontsize=7, color=INK,
                        va="top", ha=side)
            if col == 1:
                grace_us = (t_clean - t_resume) // 1000
                ax.axvspan(0, (t_clean - ref) / scale, facecolor="none", edgecolor=NEUTRAL, hatch="///", lw=0, zorder=1)
                ax.text((t_clean - ref) / scale / 2, ymin + (ymax - ymin) * 0.12, f"resume grace\n{grace_us} µs",
                        fontsize=7.5, ha="center", va="bottom", color=INK,
                        bbox={"facecolor": SURFACE, "edgecolor": "none", "pad": 1.5})
            ax.set_xlabel(f"time from {'the pause' if col == 0 else 'the resumption'} ({unit})")
            ax.set_ylabel("price ($)")
            hh = int(t_resume // 3_600_000_000_000)
            mm = int(t_resume // 60_000_000_000 % 60)
            ss = (t_resume % 60_000_000_000) / 1e9
            ax.set_title(f"pause {k + 1}: resumed {hh:02d}:{mm:02d}:{ss:09.6f}"
                         + (" — whole pause" if col == 0 else " — zoom ±400 µs"), loc="left", fontsize=9)
            if k == 0 and col == 0:
                ax.legend(loc="lower left", frameon=False, fontsize=8)
    fig.suptitle("SXTC, 2019-01-30: LULD pause -> book crosses (gray) -> resumes crossed -> resume grace (hatched) -> clean",
                 x=0.01, ha="left", fontsize=10)
    fig.subplots_adjust(left=0.07, right=0.99, top=0.90, bottom=0.12, hspace=0.55, wspace=0.18)
    caption(fig, notes)
    save(fig, "sxtc_resume")


if __name__ == "__main__":
    latency()
    throughput()
    sxtc()
    print("charts written to", OUT)
