#!/usr/bin/env python3
"""Aggregates raw lob_bench CSV runs into Markdown tables (BENCHMARK.md §3).

  bench_report.py DIR                 tables for one set of runs
  bench_report.py DIR_A DIR_B         A/B comparison

Per scenario: throughput = median over the repetitions of one run, then the
median over runs, with the min..max range of the per-run medians. Latency
percentiles: median over runs, plus the min..max range of p99 over runs.
In comparisons a difference is called "within noise" when the per-run
throughput ranges of A and B overlap.
"""
import csv
import pathlib
import statistics
import sys
from collections import defaultdict


def load(directory):
    runs = []
    for path in sorted(pathlib.Path(directory).glob("run_*.csv")):
        data = defaultdict(lambda: defaultdict(list))
        env = {}
        with open(path, newline="") as f:
            for row in csv.reader(f):
                if len(row) != 4:
                    continue
                scenario, metric, _rep, value = row
                if scenario == "env":
                    env[metric] = value
                    continue
                data[scenario][metric].append(float(value))
        runs.append((data, env))
    return runs


def per_run(runs, scenario, metric, reduce=statistics.median):
    out = []
    for data, _ in runs:
        values = data.get(scenario, {}).get(metric)
        if values:
            out.append(reduce(values))
    return out


def fmt_rate(v):
    if v >= 1e6:
        return f"{v / 1e6:.2f} M/s"
    if v >= 1e3:
        return f"{v / 1e3:.1f} k/s"
    return f"{v:.0f} /s"


def scenarios(runs):
    names = []
    for data, _ in runs:
        for s in data:
            if s not in names:
                names.append(s)
    return names


def table_single(runs):
    lines = [f"runs: {len(runs)}; clock pair (median of runs): "
             f"{statistics.median(float(e.get('clock_pair_ns', 'nan')) for _, e in runs):.0f} ns", ""]
    lines.append("| scenario | throughput (median of runs) | range over runs | p50 ns | p95 ns | p99 ns | p99 range | p99.9 ns | max ns | samples |")
    lines.append("|---|---|---|---|---|---|---|---|---|---|")
    for s in scenarios(runs):
        thr = per_run(runs, s, "throughput")
        p = {m: per_run(runs, s, m) for m in ("lat_p50", "lat_p95", "lat_p99", "lat_p999", "lat_max", "samples")}
        if not thr and not p["lat_p50"]:
            continue
        med = lambda xs: f"{statistics.median(xs):.0f}" if xs else "–"
        span = lambda xs: f"{min(xs):.0f}..{max(xs):.0f}" if xs else "–"
        thr_med = fmt_rate(statistics.median(thr)) if thr else "–"
        thr_span = f"{fmt_rate(min(thr))} .. {fmt_rate(max(thr))}" if thr else "–"
        lines.append(
            f"| {s} | {thr_med} | {thr_span} | {med(p['lat_p50'])} | {med(p['lat_p95'])} | "
            f"{med(p['lat_p99'])} | {span(p['lat_p99'])} | {med(p['lat_p999'])} | "
            f"{med(p['lat_max'])} | {med(p['samples'])} |")
    return "\n".join(lines)


def table_compare(a, b):
    lines = [f"A runs: {len(a)}, B runs: {len(b)}", ""]
    lines.append("| scenario | A throughput (range) | B throughput (range) | B vs A | verdict | A p50/p99/p99.9 ns | B p50/p99/p99.9 ns |")
    lines.append("|---|---|---|---|---|---|---|")
    for s in scenarios(a):
        ta, tb = per_run(a, s, "throughput"), per_run(b, s, "throughput")
        la = [per_run(a, s, m) for m in ("lat_p50", "lat_p99", "lat_p999")]
        lb = [per_run(b, s, m) for m in ("lat_p50", "lat_p99", "lat_p999")]
        lat = lambda ls: "/".join(f"{statistics.median(x):.0f}" if x else "–" for x in ls)
        if ta and tb:
            ma, mb = statistics.median(ta), statistics.median(tb)
            change = (mb - ma) / ma * 100
            overlap = min(tb) <= max(ta) and min(ta) <= max(tb)
            verdict = "within noise" if overlap else ("faster" if mb > ma else "slower")
            lines.append(f"| {s} | {fmt_rate(ma)} ({fmt_rate(min(ta))}..{fmt_rate(max(ta))}) | "
                         f"{fmt_rate(mb)} ({fmt_rate(min(tb))}..{fmt_rate(max(tb))}) | {change:+.1f}% | {verdict} | "
                         f"{lat(la)} | {lat(lb)} |")
        elif la[0] and lb[0]:
            lines.append(f"| {s} | – | – | – | – | {lat(la)} | {lat(lb)} |")
    return "\n".join(lines)


if __name__ == "__main__":
    if len(sys.argv) == 2:
        print(table_single(load(sys.argv[1])))
    elif len(sys.argv) == 3:
        print(table_compare(load(sys.argv[1]), load(sys.argv[2])))
    else:
        sys.exit(__doc__)
