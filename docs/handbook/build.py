#!/usr/bin/env python3
"""Build the two printable review handbooks (entry point: docs/handbook/build.sh).

    content/zh.md, content/en.md   the text; same outline, same section numbers
    numbers.yaml                   every number, with its source ({{n:key}})
    diagrams.py                    the figures ({{fig:name|caption}})
    style.css                      A4 print layout

Checks (the build fails on any of them):
  - both editions have exactly the same numbered headings;
  - every {{n:...}} / {{fig:...}} placeholder resolves;
  - the required fonts are installed, and every character of the text has a
    glyph in one of them (no tofu boxes);
  - every font in the produced PDFs is embedded (pdffonts).
Afterwards every page is rendered to PNG in _render/ for a visual check.
"""

from __future__ import annotations

import datetime
import pathlib
import re
import shutil
import subprocess
import sys

import markdown
import yaml
from fontTools.ttLib import TTCollection, TTFont
from weasyprint import HTML

import diagrams

HERE = pathlib.Path(__file__).resolve().parent
FONTS = ["Noto Serif CJK SC", "Noto Sans CJK SC", "DejaVu Sans Mono", "Noto Sans Mono CJK SC"]
OUT = {"zh": HERE / "handbook-zh.pdf", "en": HERE / "handbook-en.pdf"}
TITLE = {"zh": "确定性限价订单簿与撮合系统 · 复习手册", "en": "Deterministic Limit Order Book & Trading System · Review Handbook"}
SHORT = {"zh": "订单簿与撮合系统 · 复习手册", "en": "Order Book & Trading System · Review Handbook"}
DAGGER = "†"


def fail(msg: str) -> None:
    print(f"build: ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


# ---------------------------------------------------------------- fonts

def font_file(family: str) -> tuple[str, int]:
    out = subprocess.run(["fc-match", "-f", "%{file}|%{index}|%{family}", family], capture_output=True, text=True).stdout
    path, index, fam = out.split("|", 2)
    if family.lower() not in fam.lower():
        fail(f"font '{family}' is not installed (fc-match gave '{fam}'). "
             "Install: sudo apt install fonts-noto-cjk fonts-dejavu-core")
    return path, int(index or 0)


def cmaps() -> list[set[int]]:
    sets = []
    for fam in FONTS:
        path, index = font_file(fam)
        font = TTCollection(path).fonts[index] if path.endswith(".ttc") else TTFont(path)
        sets.append(set(font.getBestCmap().keys()))
    return sets


def check_glyphs(text: str, sets: list[set[int]], lang: str) -> None:
    missing = sorted({c for c in text if not c.isspace() and not any(ord(c) in s for s in sets)})
    if missing:
        fail(f"{lang}: characters without a glyph in {FONTS}: " + " ".join(f"{c!r}(U+{ord(c):04X})" for c in missing))


# ---------------------------------------------------------------- content

def load_numbers() -> dict:
    data = yaml.safe_load((HERE / "numbers.yaml").read_text(encoding="utf-8"))
    for key, v in data["numbers"].items():
        for field in ("zh", "en", "source", "wallclock"):
            if field not in v:
                fail(f"numbers.yaml: {key} has no '{field}'")
    return data


def wallclock_state(nums: dict) -> tuple[str, str]:
    """('pending' | 'partial' | 'local', date of the latest local measurement)."""
    origins = [v.get("measured_on", "") for v in nums.values() if v["wallclock"] and not v.get("kept")]
    local = [o for o in origins if o.startswith("local")]
    dates = sorted(re.findall(r"\d{4}-\d{2}-\d{2}", " ".join(local)))
    state = "local" if local and len(local) == len(origins) else ("partial" if local else "pending")
    return state, (dates[-1] if dates else "")


def footer_note(data: dict, lang: str) -> str:
    state, date = wallclock_state(data["numbers"])
    if state == "local":
        return f"{DAGGER} 墙钟数字：本机实测（{date}）" if lang == "zh" else f"{DAGGER} wall-clock number: measured locally ({date})"
    if state == "partial":
        return (f"{DAGGER} 墙钟数字：部分已本机复测，来源见附录 A" if lang == "zh"
                else f"{DAGGER} wall-clock number: partly re-measured locally, origin in App. A")
    return (f"{DAGGER} 墙钟数字：待本机复测（附录 B）" if lang == "zh"
            else f"{DAGGER} wall-clock number: pending local re-measurement (App. B)")


def t2t_values(nums: dict):
    def us(key):
        label = re.search(r"[\d.]+", nums[key]["en"]).group(0)
        return float(label), label
    # Figure 5: the same loopback measurement on the two machines it ran on.
    return [(us("t2t_loop_first_cloud"), us("t2t_loop_first_busy_cloud")),
            (us("t2t_loop_first"), us("t2t_loop_first_busy"))]


def expand(src: str, lang: str, data: dict) -> str:
    nums = data["numbers"]

    def num(m):
        key = m.group(1)
        if key not in nums:
            fail(f"{lang}: unknown number '{key}'")
        v = nums[key]
        mark = f'<sup class="wc">{DAGGER}</sup>' if v["wallclock"] else ""
        return f"{v[lang]}{mark}"

    def fig(m):
        name, caption = m.group(1), m.group(2)
        if name == "t2t":
            svg = diagrams.fig_t2t(lang, t2t_values(nums))
        else:
            fn = getattr(diagrams, f"fig_{name}", None)
            if fn is None:
                fail(f"{lang}: unknown figure '{name}'")
            svg = fn(lang)
        return f'\n<figure>{svg}<figcaption>{caption}</figcaption></figure>\n'

    def table(m):
        if m.group(1) != "wallclock":
            fail(f"{lang}: unknown table '{m.group(1)}'")
        head = "| 数字 | 当前值 | 出处 |" if lang == "zh" else "| Number | Value | Source |"
        groups: dict[str, list[str]] = {}
        for key, v in nums.items():
            if not v["wallclock"]:
                continue
            if f"label_{lang}" not in v:
                fail(f"numbers.yaml: wall-clock number {key} has no label_{lang}")
            origin = v.get("measured_on", "")
            origin = re.sub(r" \(.*\)$", "", origin)  # group by machine and date, details stay in the yaml
            src_short = re.sub(r" \([^()]*\)", "", v["source"])
            groups.setdefault(origin, []).append(f"| {v['label_' + lang]} | {v[lang]} | {src_short} |")
        out = []
        # "12th Gen Intel(R) Core(TM) i7-12700H, 20 CPUs, Linux 6.6...-WSL2" -> "Intel i7-12700H, 20 CPUs, WSL2"
        env = re.sub(r"\d+th Gen Intel\(R\) Core\(TM\) ", "Intel ", data["meta"].get("wallclock_local_env") or "")
        env = re.sub(r", Linux \S*WSL2$", ", WSL2", env)
        for origin, rows in groups.items():
            if origin.startswith("local ") and env:
                origin = (f"本机（{env.replace(', ', '，')}），{origin[6:]}" if lang == "zh"
                          else f"local machine ({env}), {origin[6:]}")
            title = (f"**测于：{origin}**" if lang == "zh" else f"**Measured on: {origin}**")
            out += ["", title, "", head, "|---|---|---|"] + rows
        return "\n".join(out) + "\n"

    src = re.sub(r"\{\{table:([a-z]+)\}\}", table, src)
    out = re.sub(r"\{\{n:([a-z0-9_]+)\}\}", num, src)
    out = re.sub(r"\{\{fig:([a-z0-9_]+)\|([^}]*)\}\}", fig, out)
    if "{{" in out:
        fail(f"{lang}: unresolved placeholder near: {out[out.index('{{'):out.index('{{') + 40]!r}")
    return out


HEADING = re.compile(r"^(#{2,3}) (\d+(?:\.\d+)?|[A-Z](?:\.\d+)?) ", re.M)


def outline(src: str) -> list[tuple[str, str]]:
    return [(m.group(1), m.group(2)) for m in HEADING.finditer(src)]


def to_html(src: str, lang: str, data: dict) -> str:
    body = markdown.markdown(src, extensions=["tables", "attr_list", "md_in_html", "fenced_code", "sane_lists"])
    toc = []

    def anchor(m):
        level, attrs, num, rest = m.group(1), m.group(2), m.group(3), m.group(4)
        sid = "s-" + num.replace(".", "-")
        if level == "2":
            toc.append((sid, f"{num} {re.sub('<[^>]+>', '', rest)}"))
        return f'<h{level} id="{sid}"{attrs}>{num} {rest}</h{level}>'

    body = re.sub(r'<h([23])([^>]*)>(\d+(?:\.\d+)?|[A-Z](?:\.\d+)?) (.*?)</h\1>', anchor, body)
    meta = data["meta"]
    today = datetime.date.today().isoformat()
    if lang == "zh":
        cover = f"""<section class="cover"><h1>{TITLE['zh'].split(' · ')[0]}</h1>
<p class="sub">复习手册：做了什么、为什么这样设计、怎么证明它是对的</p>
<div class="meta">范围：L1 核心撮合 · L2 性能工程 · L3′ 行情与流水线 · L4 下单链路（L5 实施中，L6 为未来计划）<br>
仓库状态：{meta['repo_state']} · 生成日期：{today}<br>
所有数字来自 docs/handbook/numbers.yaml（每项注明出处）；带 {DAGGER} 的是墙钟数字。<br>
配套英文版 handbook-en.pdf 使用同一大纲与章节编号。</div></section>"""
        toc_title, toc_note = "目录", (f"读法：先读第 0、1 章（两分钟讲完整个项目），再按面试岗位挑章节；第 9、10 章是答问材料。"
                                      f"{DAGGER} 的含义见每页页脚。")
    else:
        cover = f"""<section class="cover"><h1>{TITLE['en'].split(' · ')[0]}</h1>
<p class="sub">Review handbook: what was built, why it is designed this way, and how we know it is right</p>
<div class="meta">Scope: L1 core matching · L2 performance engineering · L3′ market data &amp; pipeline · L4 order entry (L5 in progress, L6 a future plan)<br>
Repository: {meta['repo_state']} · generated {today}<br>
Every number comes from docs/handbook/numbers.yaml, with its source; {DAGGER} marks wall-clock numbers.<br>
The Chinese edition (handbook-zh.pdf) uses the same outline and section numbers.</div></section>"""
        toc_title, toc_note = "Contents", (f"How to read: chapters 0 and 1 give the two-minute story; then pick chapters by role. "
                                          f"Chapters 9 and 10 are interview material. The footer explains {DAGGER}.")
    toc_html = "".join(f'<li><a href="#{sid}">{label}</a></li>' for sid, label in toc)
    toc_block = f'<nav class="toc"><h2>{toc_title}</h2><ol>{toc_html}</ol><p class="note">{toc_note}</p></nav>'
    css = (HERE / "style.css").read_text(encoding="utf-8").replace("__FOOTER__", footer_note(data, lang).replace('"', "'"))
    css = css.replace("__DOCTITLE__", SHORT[lang])
    lang_attr = "zh-CN" if lang == "zh" else "en"
    return (f'<!DOCTYPE html><html lang="{lang_attr}"><head><meta charset="utf-8"><title>{TITLE[lang]}</title>'
            f'<meta name="author" content="zkingom/quant"><style>{css}</style></head><body>'
            f'{cover}{toc_block}<main>{body}</main></body></html>')


def visible_text(html: str) -> str:
    html = re.sub(r"<style>.*?</style>", "", html, flags=re.S)
    return re.sub(r"<[^>]+>", " ", html)


# ---------------------------------------------------------------- main

def main() -> None:
    data = load_numbers()
    srcs = {lang: (HERE / "content" / f"{lang}.md").read_text(encoding="utf-8") for lang in ("zh", "en")}
    if outline(srcs["zh"]) != outline(srcs["en"]):
        zh, en = outline(srcs["zh"]), outline(srcs["en"])
        diff = [(a, b) for a, b in zip(zh, en) if a != b] or [("length", (len(zh), len(en)))]
        fail(f"the two editions do not share the same outline: first difference {diff[0]}")
    sets = cmaps()
    render = HERE / "_render"
    shutil.rmtree(render, ignore_errors=True)
    render.mkdir()
    for lang in ("zh", "en"):
        html = to_html(expand(srcs[lang], lang, data), lang, data)
        check_glyphs(visible_text(html) + footer_note(data, lang) + TITLE[lang], sets, lang)
        (render / f"{lang}.html").write_text(html, encoding="utf-8")
        HTML(string=html, base_url=str(HERE)).write_pdf(OUT[lang])
        fonts = subprocess.run(["pdffonts", str(OUT[lang])], capture_output=True, text=True).stdout.splitlines()[2:]
        not_embedded = [f for f in fonts if f.split()[-5] != "yes"]
        if not fonts or not_embedded:
            fail(f"{lang}: fonts not embedded: {not_embedded}")
        pages = int(re.search(r"Pages:\s+(\d+)", subprocess.run(["pdfinfo", str(OUT[lang])], capture_output=True, text=True).stdout).group(1))
        subprocess.run(["pdftoppm", "-r", "60", "-png", str(OUT[lang]), str(render / lang)], check=True)
        print(f"build: {OUT[lang].name}: {pages} pages, fonts embedded: " + ", ".join(sorted({f.split()[0].split('+')[-1] for f in fonts})))
    print(f"build: page images for the visual check in {render}/")


if __name__ == "__main__":
    main()
