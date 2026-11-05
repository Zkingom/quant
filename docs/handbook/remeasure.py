#!/usr/bin/env python3
"""Re-measure the handbooks' wall-clock numbers on this machine and write them
into numbers.yaml (entry point: docs/handbook/remeasure.sh).

    remeasure.py --bin BUILD_DIR [--itch FILE] [--l2-summary FILE]
    remeasure.py --from-log LOG      import runs made by hand (see below)

Runs (pinned when the machine has enough cores):
  lob_bench --filter engine --reps 5           engine throughput / latency, p99 range over profiles
  lob_itch_bench --messages 1000000 --reps 5   ITCH replay hops, handoff, risk check, CRTP vs virtual
  lob_t2t  x3, lob_t2t --busy-poll x3          tick-to-trade (medians of the runs)
  lob_itch_market --bench-bytes 3e9 (--itch)   market-wide throughput on a real ITCH file
and reads the summary written by scripts/l2_reproduce.sh (--l2-summary) for E1.

--from-log takes a log of those same commands run by hand: every block that
starts with a "$ ... lob_itch_bench ..." or "$ ... lob_t2t ..." line, up to
the next "$ " or "=====" line, is parsed exactly like a live run; several runs
of one command are combined by their median (the p99 spread becomes the range
of the per-run spreads). The measurement date and machine are taken from the
log ("Date:" line, and the environment lob_t2t prints).

Only the measured keys change; their `measured_on` becomes "local YYYY-MM-DD"
and meta.wallclock_local_env records the machine. Entries marked `kept: true`
(measurements of a named environment kept for comparison) are never touched. Lines are edited in place,
so the comments and layout of numbers.yaml are kept.
"""

from __future__ import annotations

import argparse
import datetime
import os
import pathlib
import platform
import re
import statistics
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
YAML = HERE / "numbers.yaml"


def run(cmd: list[str]) -> str:
    print("remeasure: $ " + " ".join(cmd), flush=True)
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        sys.exit(f"remeasure: command failed ({res.returncode}):\n{res.stderr[-2000:]}")
    return res.stdout + res.stderr


def pin(threads: int) -> list[str]:
    """Pin to distinct cores that exist: 2,4 on larger machines (BENCHMARK's
    choice on the author's WSL2), 1,2 on a 4-CPU machine, none below that."""
    n = os.cpu_count() or 1
    if n < 4 or not shutil_which("taskset"):
        return []
    cpus = ["2", "4"] if n >= 6 else ["1", "2"]
    return ["taskset", "-c", ",".join(cpus[:threads])]


def shutil_which(name: str) -> bool:
    return any((pathlib.Path(p) / name).exists() for p in os.environ.get("PATH", "").split(":"))


def fmt(x: float) -> str:
    """0.46 / 9.1 / 28: two decimals below 1, one below 10, none above."""
    return f"{x:.2f}" if x < 1 else (f"{x:.1f}" if x < 10 else f"{x:.0f}")


# ---------------------------------------------------------------- measurements

def engine(bin_dir: pathlib.Path) -> dict:
    out = run(pin(1) + [str(bin_dir / "bench/lob_bench"), "--filter", "engine", "--reps", "5"])
    tput, p50, p99 = {}, {}, {}
    for line in out.splitlines():
        parts = line.split(",")
        if len(parts) != 4 or "[" in parts[0]:
            continue
        name, metric, _rep, value = parts
        if metric == "throughput":
            tput.setdefault(name, []).append(float(value))
        elif metric == "lat_p50":
            p50[name] = float(value)
        elif metric == "lat_p99":
            p99[name] = float(value)
    mixed = statistics.median(tput["engine.mixed"]) / 1e6
    p99s = [v for k, v in p99.items() if k.startswith("engine.")]
    lo, hi = min(p99s) / 1000, max(p99s) / 1000
    return {
        "engine_mixed_tput": (f"{mixed:.1f} M 条/s", f"{mixed:.1f} M commands/s"),
        "engine_mixed_p50": (f"{p50['engine.mixed']:.0f} ns",) * 2,
        "engine_mixed_p99": (f"{p99['engine.mixed']:.0f} ns",) * 2,
        "engine_p99_range": (f"{lo:.1f}–{hi:.1f} µs",) * 2,
    }


def table_rows(out: str) -> list[list[str]]:
    return [[c.strip() for c in line.strip().strip("|").split("|")] for line in out.splitlines() if line.startswith("| ")]


def itch_parse(out: str) -> dict:
    hop, tput, disp = {}, {}, {}
    for r in table_rows(out):
        if len(r) == 7 and r[0] in ("single", "pipeline", "paced"):
            hop[(r[0], r[1])] = r
        elif len(r) == 3 and r[0] in ("single", "pipeline", "paced"):
            tput[r[0]] = float(r[1])
        elif len(r) == 3 and r[0] in ("CRTP (static)", "virtual", "risk check_new"):
            disp[r[0]] = float(r[1])
    lo, hi = (float(x) for x in hop[("paced", "queue")][6].split("–"))
    return {
        "single": tput["single"] / 1e6,
        "decode_p50": float(hop[("single", "decode")][2]), "decode_p99": float(hop[("single", "decode")][3]),
        "book_p50": float(hop[("single", "book")][2]), "book_p99": float(hop[("single", "book")][3]),
        "handoff_p50": float(hop[("paced", "queue")][2]),
        "spread": hi / lo if lo > 0 else 0.0,
        "queue_ms": float(hop[("pipeline", "queue")][2]) / 1e6,
        "risk": disp["risk check_new"], "crtp": disp["CRTP (static)"], "virtual": disp["virtual"],
    }


def itch_format(runs: list[dict]) -> dict:
    m = {k: statistics.median(r[k] for r in runs) for k in runs[0]}
    s_lo, s_hi = min(r["spread"] for r in runs), max(r["spread"] for r in runs)
    spread = fmt(s_lo) if fmt(s_lo) == fmt(s_hi) else f"{fmt(s_lo)}–{fmt(s_hi)}"
    ns = lambda k: (f"{m[k]:.0f} ns",) * 2  # noqa: E731
    return {
        "itch_single_tput": (f"{m['single']:.1f} M 条/s", f"{m['single']:.1f} M messages/s"),
        "decode_p50": ns("decode_p50"), "decode_p99": ns("decode_p99"),
        "book_p50": ns("book_p50"), "book_p99": ns("book_p99"),
        "handoff_p50": ns("handoff_p50"),
        "handoff_spread": (f"{spread} 倍", f"{spread}x"),
        "queue_saturated": (f"约 {m['queue_ms']:.1f} ms", f"about {m['queue_ms']:.1f} ms"),
        "risk_check_ns": (f"≈ {m['risk']:.0f} ns", f"about {m['risk']:.0f} ns"),
        "crtp_ns": (f"{m['crtp']:.1f}",) * 2,
        "virtual_ns": (f"{m['virtual']:.1f}",) * 2,
    }


def itch(bin_dir: pathlib.Path) -> dict:
    out = run(pin(2) + [str(bin_dir / "bench/lob_itch_bench"), "--messages", "1000000", "--reps", "5"])
    return itch_format([itch_parse(out)])


def t2t_parse(out: str) -> dict[tuple[str, str], float]:
    rows = {}
    for r in table_rows(out):
        if len(r) == 7 and r[0] in ("inproc", "loopback", "ack"):
            rows[(r[0], r[1])] = float(r[3]) / 1000  # p50 in µs
    return rows


def t2t_run(bin_dir: pathlib.Path, busy: bool) -> dict[tuple[str, str], float]:
    # README / BENCHMARK §15 pin the client to CPU 1 and the exchange to CPU 2.
    cmd = [str(bin_dir / "bench/lob_t2t")]
    if (os.cpu_count() or 1) >= 4:
        cmd += ["--client-cpu", "1", "--server-cpu", "2"]
    if busy:
        cmd.append("--busy-poll")
    return t2t_parse(run(cmd))


def t2t_format(sleep: list[dict], busy: list[dict]) -> dict:
    def med(runs, mode, seg_prefix):
        vals = [v for r in runs for (m, s), v in r.items() if m == mode and s.startswith(seg_prefix)]
        return statistics.median(vals)

    first_in = med(sleep, "inproc", "T2 tick -> encoded, 1st")
    first_lo = med(sleep, "loopback", "T3 tick -> send() ret, 1st")
    first_bp = med(busy, "loopback", "T3 tick -> send() ret, 1st")
    rtt, rtt_bp = med(sleep, "ack", "T5"), med(busy, "ack", "T5")
    wire = med(sleep, "loopback", "T4")
    send = med(sleep, "loopback", "T3 tick -> send() ret, new") - med(sleep, "loopback", "T1 tick -> decision")
    us = lambda x: f"{fmt(x)} µs"  # noqa: E731
    return {
        "t2t_inproc_first": (us(first_in),) * 2,
        "t2t_loop_first": (us(first_lo),) * 2,
        "t2t_loop_first_busy": (us(first_bp),) * 2,
        "t2t_rtt": (us(rtt),) * 2,
        "t2t_rtt_busy": (us(rtt_bp),) * 2,
        "t2t_wire": (us(wire),) * 2,
        "send_cost": (f"约 {fmt(send)} µs", f"about {fmt(send)} µs"),
    }


def t2t(bin_dir: pathlib.Path) -> dict:
    return t2t_format([t2t_run(bin_dir, False) for _ in range(3)], [t2t_run(bin_dir, True) for _ in range(3)])


def from_log(path: str) -> tuple[dict, str, str]:
    """Results, date and machine of a log of hand-run lob_itch_bench / lob_t2t commands."""
    text = pathlib.Path(path).read_text(encoding="utf-8")
    itch_runs, sleep, busy = [], [], []
    for m in re.finditer(r"^\$ ([^\n]*)\n(.*?)(?=^\$ |^=====|\Z)", text, re.M | re.S):
        argv, out = m.group(1).split(), m.group(2)
        if argv[:1] == ["taskset"]:
            argv = argv[3:]
        prog = pathlib.Path(argv[0]).name if argv else ""
        if prog == "lob_itch_bench":
            itch_runs.append(itch_parse(out))
        elif prog == "lob_t2t":
            (busy if "--busy-poll" in argv else sleep).append(t2t_parse(out))
    results = {}
    if itch_runs:
        results.update(itch_format(itch_runs))
    if sleep and busy:
        results.update(t2t_format(sleep, busy))
    if not results:
        sys.exit(f"remeasure: no lob_itch_bench / lob_t2t runs found in {path}")
    print(f"remeasure: {path}: {len(itch_runs)} lob_itch_bench, {len(sleep)} lob_t2t, {len(busy)} lob_t2t --busy-poll runs")
    date = re.search(r"^Date: (\d{4}-\d{2}-\d{2})", text, re.M)
    cpu = re.search(r"^\s*cpu: (.+)$", text, re.M)
    sysinfo = re.search(r"^\s*online cpus: (\d+); kernel: ([^;]+);", text, re.M)
    env = (f"{cpu.group(1).strip()}, {sysinfo.group(1)} CPUs, {sysinfo.group(2).strip()}"
           if cpu and sysinfo else "machine of " + pathlib.Path(path).name)
    return results, (date.group(1) if date else datetime.date.today().isoformat()), env


def market(bin_dir: pathlib.Path, itch_file: str) -> dict:
    out = run([str(bin_dir / "apps/lob_itch_market"), "--input", itch_file, "--bench-bytes", "3000000000"])
    fps = {m.group(1): float(m.group(2)) / 1e6 for m in re.finditer(r"\[(single|shards=4)\].*?frames_per_s=(\d+)", out)}
    return {
        "mkt_single": (f"{fps['single']:.2f} M 帧/s", f"{fps['single']:.2f} M frames/s"),
        "mkt_n4": (f"{fps['shards=4']:.2f} M 帧/s", f"{fps['shards=4']:.2f} M frames/s"),
    }


def l2(summary: str) -> dict:
    text = pathlib.Path(summary).read_text(encoding="utf-8")

    def row(name):
        m = re.search(rf"^\| {re.escape(name)} \| ([^|]+)\| ([^|]+)\|.*$", text, re.M)
        return m.group(0).split("|")

    def rate(cell):  # "4.9 k/s (3.9 k/s..5.2 k/s)" -> per second
        v, unit = re.match(r"\s*([\d.]+) ([kM]?)/s", cell).groups()
        return float(v) * {"": 1, "k": 1e3, "M": 1e6}[unit]

    mem = row("session.memory")
    a, b = rate(mem[2]), rate(mem[3])
    rep = row("replay.session_memory")
    la, lb = float(rep[6].split("/")[0]) / 1000, float(rep[7].split("/")[0]) / 1000
    return {
        "e1_before": (f"{a / 1e3:.1f} k 条/s", f"{a / 1e3:.1f} k commands/s"),
        "e1_after": (f"{b / 1e6:.2f} M 条/s", f"{b / 1e6:.2f} M commands/s"),
        "e1_lat_before": (f"{la:.0f} µs",) * 2,
        "e1_lat_after": (f"{lb:.2f} µs",) * 2,
        "e1_ratio": (f"约 {b / a:.0f} 倍", f"{b / a:.0f} times"),
    }


# ---------------------------------------------------------------- numbers.yaml

def set_value(text: str, key: str, zh: str, en: str, origin: str) -> str:
    line_re = re.compile(rf"^(  {key}: \{{)(.*)(\}})$", re.M)
    m = line_re.search(text)
    if not m:
        sys.exit(f"remeasure: {key} not found in numbers.yaml")
    body = m.group(2)
    # (?<![\w]) keeps label_zh / label_en untouched
    body = re.sub(r'(?<![\w])zh: "[^"]*"', f'zh: "{zh}"', body)
    body = re.sub(r'(?<![\w])en: "[^"]*"', f'en: "{en}"', body)
    body = re.sub(r'(?<![\w])measured_on: "[^"]*"', f'measured_on: "{origin}"', body)
    return text[:m.start()] + m.group(1) + body + m.group(3) + text[m.end():]


def environment() -> str:
    cpu = "?"
    try:
        for line in pathlib.Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                cpu = line.split(":", 1)[1].strip()
                break
    except OSError:
        pass
    return f"{cpu}, {os.cpu_count()} CPUs, {platform.system()} {platform.release()}"


def main() -> None:
    ap = argparse.ArgumentParser()
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--bin", help="build directory with the benchmark binaries (measure now)")
    src.add_argument("--from-log", help="log of hand-run lob_itch_bench / lob_t2t commands")
    ap.add_argument("--itch")
    ap.add_argument("--l2-summary")
    ap.add_argument("--yaml", default=str(YAML), help="numbers file to update (tests use a copy)")
    args = ap.parse_args()
    yaml_path = pathlib.Path(args.yaml)
    results = {}
    if args.from_log:
        found, day, env = from_log(args.from_log)
        results.update(found)
    else:
        bin_dir = pathlib.Path(args.bin)
        day, env = datetime.date.today().isoformat(), environment()
        results.update(engine(bin_dir))
        results.update(itch(bin_dir))
        results.update(t2t(bin_dir))
        if args.itch:
            results.update(market(bin_dir, args.itch))
    if args.l2_summary:
        results.update(l2(args.l2_summary))
    text = yaml_path.read_text(encoding="utf-8")
    for key, vals in results.items():
        zh, en = vals if len(vals) == 2 else (vals[0], vals[0])
        text = set_value(text, key, zh, en, f"local {day}")
        print(f"remeasure: {key:22s} {en}")
    text = re.sub(r"^  wallclock_local_env: .*$", f'  wallclock_local_env: "{env}"', text, flags=re.M)
    yaml_path.write_text(text, encoding="utf-8")
    left = [k for k, v in __import__("yaml").safe_load(text)["numbers"].items()
            if v["wallclock"] and not v.get("kept") and not str(v.get("measured_on", "")).startswith("local")]
    if left:
        print("remeasure: not re-measured in this run (still from their original source): " + ", ".join(left))


if __name__ == "__main__":
    main()
