"""SVG figures for the review handbooks (one function per figure, two languages).

Print rules: black lines on white, no colour needed to tell things apart.
Categories differ by line style (solid / dashed / double) or by hatching;
every figure has a legend when it uses more than one style.
"""

from __future__ import annotations

from html import escape

FONT = "Noto Sans CJK SC, DejaVu Sans, sans-serif"
MONO = "DejaVu Sans Mono, monospace"

DEFS = """
<defs>
  <marker id="arr" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse">
    <path d="M0,0 L10,5 L0,10 z" fill="#000"/>
  </marker>
  <pattern id="hatch" width="6" height="6" patternUnits="userSpaceOnUse" patternTransform="rotate(45)">
    <line x1="0" y1="0" x2="0" y2="6" stroke="#000" stroke-width="1"/>
  </pattern>
  <pattern id="dots" width="5" height="5" patternUnits="userSpaceOnUse">
    <circle cx="2.5" cy="2.5" r="0.9" fill="#000"/>
  </pattern>
  <pattern id="grey" width="4" height="4" patternUnits="userSpaceOnUse">
    <rect width="4" height="4" fill="#fff"/><rect width="2" height="2" fill="#bbb"/><rect x="2" y="2" width="2" height="2" fill="#bbb"/>
  </pattern>
</defs>
"""


def _svg(w: int, h: int, body: str, title: str) -> str:
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="100%" '
            f'role="img" aria-label="{escape(title)}">{DEFS}{body}</svg>')


def text(x, y, s, size=11, anchor="middle", weight="normal", family=FONT, italic=False):
    st = ' font-style="italic"' if italic else ""
    return (f'<text x="{x}" y="{y}" font-family="{family}" font-size="{size}" text-anchor="{anchor}" '
            f'font-weight="{weight}"{st} fill="#000">{escape(s)}</text>')


def text_bg(x, y, s, size=11, anchor="middle", weight="normal", italic=False):
    """Text on a white background (for labels that sit on lines)."""
    w = sum(size * (1.0 if ord(c) > 0x2E80 else 0.56) for c in s) + 6
    x0 = {"middle": x - w / 2, "start": x - 3, "end": x - w + 3}[anchor]
    return (f'<rect x="{x0:.1f}" y="{y - size:.1f}" width="{w:.1f}" height="{size * 1.35:.1f}" fill="#fff"/>'
            + text(x, y, s, size, anchor, weight, italic=italic))


def box(x, y, w, h, lines, size=11, style="solid", fill="#fff", bold_first=False, rx=4):
    dash = {"solid": "", "dashed": ' stroke-dasharray="6 3"', "dotted": ' stroke-dasharray="2 2"'}.get(style, "")
    out = []
    if style == "double":
        out.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}" fill="{fill}" stroke="#000" stroke-width="1.2"/>')
        out.append(f'<rect x="{x+3}" y="{y+3}" width="{w-6}" height="{h-6}" rx="{max(rx-2,0)}" fill="none" stroke="#000" stroke-width="0.8"/>')
    else:
        out.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}" fill="{fill}" stroke="#000" stroke-width="1.2"{dash}/>')
    n = len(lines)
    lh = size * 1.3
    y0 = y + h / 2 - (n - 1) * lh / 2 + size * 0.35
    for i, s in enumerate(lines):
        out.append(text(x + w / 2, y0 + i * lh, s, size, weight="bold" if (bold_first and i == 0) else "normal"))
    return "".join(out)


def arrow(x1, y1, x2, y2, style="solid", width=1.3, both=False):
    dash = ' stroke-dasharray="6 3"' if style == "dashed" else ""
    start = ' marker-start="url(#arr)"' if both else ""
    return (f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="#000" stroke-width="{width}"{dash}'
            f' marker-end="url(#arr)"{start}/>')


def poly_arrow(points, style="solid"):
    dash = ' stroke-dasharray="6 3"' if style == "dashed" else ""
    pts = " ".join(f"{x},{y}" for x, y in points)
    return f'<polyline points="{pts}" fill="none" stroke="#000" stroke-width="1.3"{dash} marker-end="url(#arr)"/>'


# --------------------------------------------------------------------------
# Figure 1: overall architecture

L1_LABELS = {
    "zh": dict(
        title="整体架构", l3="L3′ 行情与流水线", l4="L4 下单链路", l1="L1 核心 / L2 性能",
        src=["ITCH 文件 / 管道", "固定缓冲 read()"], dec=["解码线程", "分帧 + itch::decode"],
        spsc="SPSC 队列", book=["订单簿线程", "FeedHandler + md::Book"],
        ts=["TradingStage", "策略 → 风控 → Ex"],
        gw=["OrderGateway", "令牌 · 状态机", "OUCH 4.2"], cs=["ClientSession", "SoupBinTCP（sans-IO）"],
        tcp="回环 TCP", srv=["EpollServer（单线程）", "ServerConnection + OuchExchange"],
        sim=["SimExchange", "乐观 / 保守", "成交模型"], eng=["L1 MatchingEngine", "（冻结，未改动）"],
        io=["io：日志 · 快照 · 恢复", "lob_replay"], l2=["L2：基准 · 剖析", "E1 · LOB_BASELINE"],
        leg_solid="进程内调用", leg_dash="网络模式（L4）", leg_double="线程边界",
    ),
    "en": dict(
        title="Overall architecture", l3="L3′ market data & pipeline", l4="L4 order entry", l1="L1 core / L2 performance",
        src=["ITCH file / pipe", "fixed-buffer read()"], dec=["decoder thread", "framing + itch::decode"],
        spsc="SPSC queue", book=["book thread", "FeedHandler + md::Book"],
        ts=["TradingStage", "strategy → risk → Ex"],
        gw=["OrderGateway", "tokens · states", "OUCH 4.2"], cs=["ClientSession", "SoupBinTCP (sans-IO)"],
        tcp="loopback TCP", srv=["EpollServer (one thread)", "ServerConnection + OuchExchange"],
        sim=["SimExchange", "optimistic /", "conservative fills"], eng=["L1 MatchingEngine", "(frozen, unchanged)"],
        io=["io: journal · snapshot", "recovery · lob_replay"], l2=["L2: benchmarks · profiling", "E1 · LOB_BASELINE"],
        leg_solid="in-process call", leg_dash="network mode (L4)", leg_double="thread boundary",
    ),
}


def fig_architecture(lang: str) -> str:
    t = L1_LABELS[lang]
    b = []
    # band labels (left column)
    for y, h, lab in ((10, 92, t["l3"]), (118, 92, t["l4"]), (226, 92, t["l1"])):
        b.append(f'<rect x="4" y="{y}" width="652" height="{h}" fill="none" stroke="#000" stroke-width="0.6" stroke-dasharray="1 3"/>')
        b.append(text(12, y + 14, lab, 10, anchor="start", weight="bold"))
    # L3'
    b.append(box(16, 32, 110, 54, t["src"], 10))
    b.append(box(146, 32, 124, 54, t["dec"], 10, style="double"))
    b.append(box(338, 32, 146, 54, t["book"], 10, style="double"))
    b.append(box(510, 32, 138, 54, t["ts"], 10, bold_first=True))
    b.append(arrow(126, 59, 146, 59))
    b.append(arrow(270, 59, 338, 59))
    b.append(text(304, 52, t["spsc"], 9))
    b.append(arrow(484, 59, 510, 59))
    # L4
    b.append(box(510, 140, 138, 54, t["gw"], 9.5, style="dashed", bold_first=True))
    b.append(box(350, 140, 130, 54, t["cs"], 9.5, style="dashed"))
    b.append(box(16, 140, 200, 54, t["srv"], 9.5, style="double"))
    b.append(arrow(579, 86, 579, 140, style="dashed"))
    b.append(arrow(510, 167, 480, 167, style="dashed"))
    b.append(arrow(350, 167, 216, 167, style="dashed", both=True))
    b.append(text(283, 160, t["tcp"], 9))
    # L1 / L2
    b.append(box(350, 248, 130, 54, t["sim"], 9.5, bold_first=True))
    b.append(box(510, 248, 138, 54, t["eng"], 9.5, bold_first=True))
    b.append(box(16, 248, 150, 54, t["io"], 9.5))
    b.append(box(182, 248, 150, 54, t["l2"], 9.5, style="dotted"))
    b.append(poly_arrow([(530, 86), (530, 106), (495, 106), (495, 232), (440, 232), (440, 248)]))
    b.append(arrow(480, 275, 510, 275))
    b.append(poly_arrow([(116, 194), (116, 222), (380, 222), (380, 248)], style="dashed"))
    # legend
    y = 334
    b.append(f'<line x1="16" y1="{y}" x2="50" y2="{y}" stroke="#000" stroke-width="1.3"/>')
    b.append(text(56, y + 4, t["leg_solid"], 9.5, anchor="start"))
    b.append(f'<line x1="200" y1="{y}" x2="234" y2="{y}" stroke="#000" stroke-width="1.3" stroke-dasharray="6 3"/>')
    b.append(text(240, y + 4, t["leg_dash"], 9.5, anchor="start"))
    b.append(box(420, y - 8, 34, 16, [], 9, style="double", rx=2))
    b.append(text(462, y + 4, t["leg_double"], 9.5, anchor="start"))
    return _svg(660, 346, "".join(b), t["title"])


# --------------------------------------------------------------------------
# Figure 2: feed handler state machine + resume grace

F2 = {
    "zh": dict(title="FeedHandler 状态机", uninit="Uninitialized", live="Live", rec="Recovering", stale="Stale",
               dir="Stock Directory", gap="序号缺口", fill="补发补齐", fail="补不齐", err=["时间戳倒退 / 订单簿拒绝 /", "无法解码"],
               snap="快照重同步", grace=["恢复宽限（D-055）", "交叉 + 进入严格时段 → 宽限", "干净且见到 Cross Trade → 严格", "超 1 s 或 1 万条 → Stale"],
               note="交易阶段（D-054）与状态正交：只有常规时段且个股为 T 时才强制永不交叉"),
    "en": dict(title="FeedHandler state machine", uninit="Uninitialized", live="Live", rec="Recovering", stale="Stale",
               dir="Stock Directory", gap="sequence gap", fill="retransmission fills it", fail="cannot fill", err=["timestamp regression / book reject /", "undecodable input"],
               snap="snapshot resync", grace=["Resume grace (D-055)", "crossed when strict begins → grace", "clean + Cross Trade seen → strict", "> 1 s or > 10,000 msgs → Stale"],
               note="Trading phase (D-054) is orthogonal: never-crossed is enforced only in regular hours while the stock trades"),
}


def fig_feed(lang: str) -> str:
    t = F2[lang]
    b = []
    b.append(box(10, 30, 120, 40, [t["uninit"]], 11, rx=18))
    b.append(box(200, 30, 120, 40, [t["live"]], 12, rx=18, style="double"))
    b.append(box(460, 30, 120, 40, [t["rec"]], 11, rx=18))
    b.append(box(460, 140, 120, 40, [t["stale"]], 12, rx=18, fill="url(#grey)"))
    b.append(arrow(130, 50, 200, 50)); b.append(text(165, 44, t["dir"], 9))
    b.append(arrow(320, 44, 460, 44)); b.append(text(390, 38, t["gap"], 9))
    b.append(arrow(460, 58, 320, 58)); b.append(text(390, 72, t["fill"], 9))
    b.append(arrow(520, 70, 520, 140)); b.append(text(526, 108, t["fail"], 9, anchor="start"))
    b.append(poly_arrow([(290, 70), (290, 160), (460, 160)]))
    b.append(text(296, 120, t["err"][0], 8.5, anchor="start")); b.append(text(296, 132, t["err"][1], 8.5, anchor="start"))
    b.append(poly_arrow([(460, 172), (240, 172), (240, 70)], style="dashed")); b.append(text(250, 186, t["snap"], 9, anchor="start"))
    b.append(f'<rect x="10" y="96" width="200" height="80" rx="4" fill="url(#hatch)" stroke="none" opacity="0.25"/>')
    b.append(box(10, 96, 200, 80, t["grace"], 9, style="dashed", fill="none", bold_first=True))
    b.append(f'<line x1="210" y1="110" x2="225" y2="70" stroke="#000" stroke-width="0.8" stroke-dasharray="2 2"/>')
    b.append(text(295, 204, t["note"], 9, italic=True))
    return _svg(590, 212, "".join(b), t["title"])


# --------------------------------------------------------------------------
# Figure 3: resume grace timeline (schematic of the SXTC case)

F3 = {
    "zh": dict(title="SXTC 停牌恢复时间线（示意）", pause="LULD 暂停：允许交叉（计数）", grace="宽限：等成交回报",
               live="严格：永不交叉", h="H 'P' 暂停", q=["Q 'H' 撮合 +", "H 'T' 恢复（同一时刻）"], c=["C 成交回报到齐", "交叉消除"],
               dur="233 µs / 33 帧", old="D-054 规则：此刻交叉 → 判 Stale（误判）", new="D-055：宽限，干净后进入严格模式",
               axis="ITCH 时间 →", leg1="交叉允许（暂停）", leg2="宽限中（不可交易）"),
    "en": dict(title="SXTC resume timeline (schematic)", pause="LULD pause: crossing allowed (counted)", grace="grace: wait for executions",
               live="strict: never crossed", h="H 'P' pause", q=["Q 'H' cross +", "H 'T' resume (same instant)"], c=["C executions arrive,", "cross cleared"],
               dur="233 µs / 33 frames", old="D-054 rule: crossed here → Stale (false alarm)", new="D-055: grace, strict once the book is clean",
               axis="ITCH time →", leg1="crossing allowed (pause)", leg2="grace (not tradable)"),
}


def fig_grace(lang: str) -> str:
    t = F3[lang]
    b = []
    y0, h = 60, 34
    xs, xg, xe = 20, 280, 450  # pause start, grace start (cross + resume), grace end (executions)
    b.append(f'<rect x="{xs}" y="{y0}" width="{xg-xs}" height="{h}" fill="url(#hatch)" stroke="#000" stroke-width="1"/>')
    b.append(f'<rect x="{xg}" y="{y0}" width="{xe-xg}" height="{h}" fill="url(#dots)" stroke="#000" stroke-width="1"/>')
    b.append(f'<rect x="{xe}" y="{y0}" width="{640-xe}" height="{h}" fill="#fff" stroke="#000" stroke-width="1"/>')
    b.append(text_bg((xs + xg) / 2, y0 + 21, t["pause"], 10))
    b.append(text_bg((xg + xe) / 2, y0 + 21, t["grace"], 9.5))
    b.append(text((xe + 640) / 2, y0 + 21, t["live"], 10))
    for x, lab in ((xs, [t["h"]]), (xg, t["q"]), (xe, t["c"])):
        b.append(f'<line x1="{x}" y1="{y0-6}" x2="{x}" y2="{y0+h+40}" stroke="#000" stroke-width="1.6"/>')
        for i, line in enumerate(lab):
            b.append(text(x + 4, y0 + h + 54 + i * 13, line, 9.5, anchor="start"))
    b.append(f'<line x1="{xg}" y1="{y0-14}" x2="{xe}" y2="{y0-14}" stroke="#000" stroke-width="1" marker-end="url(#arr)" marker-start="url(#arr)"/>')
    b.append(text_bg((xg + xe) / 2, y0 - 18, t["dur"], 9.5, weight="bold"))
    b.append(text(640, 150, t["axis"], 9, anchor="end"))
    b.append(text(20, 20, t["old"], 10, anchor="start", italic=True))
    b.append(text(20, 36, t["new"], 10, anchor="start", weight="bold"))
    b.append(f'<rect x="20" y="166" width="22" height="12" fill="url(#hatch)" stroke="#000" stroke-width="0.8"/>')
    b.append(text(48, 176, t["leg1"], 9, anchor="start"))
    b.append(f'<rect x="250" y="166" width="22" height="12" fill="url(#dots)" stroke="#000" stroke-width="0.8"/>')
    b.append(text(278, 176, t["leg2"], 9, anchor="start"))
    return _svg(650, 186, "".join(b), t["title"])


# --------------------------------------------------------------------------
# Figure 4: lockstep quiescence (barrier) sequence

F4 = {
    "zh": dict(title="锁步模式的静默屏障", client="客户端（订单簿线程）", srv="交易所（epoll 线程）",
               s1="① 订单（TCP，OUCH 'O'）", s2="② 屏障 {账户, 本连接已发 n 条}（控制队列）",
               s3="③ 处理完第 n 条后回复：出站序号水位 K", s4="④ Accepted / Executed …（TCP，序号 < K）",
               s5="⑤ 客户端读到序号 K 为止 → 本步回报全部到齐", note="为什么不等'下一条回报'：多余的撤单按规范没有任何回报"),
    "en": dict(title="Lockstep quiescence barrier", client="client (book thread)", srv="exchange (epoll thread)",
               s1="① order (TCP, OUCH 'O')", s2="② barrier {account, n sent on this connection} (control queue)",
               s3="③ after processing message n: reply with outbound watermark K", s4="④ Accepted / Executed … (TCP, sequence < K)",
               s5="⑤ client reads until its next sequence is K → this step is complete", note="Why not wait for 'the next reply': a superfluous cancel gets no reply at all"),
}


def fig_lockstep(lang: str) -> str:
    t = F4[lang]
    b = []
    b.append(box(40, 8, 170, 30, [t["client"]], 10, bold_first=True))
    b.append(box(450, 8, 170, 30, [t["srv"]], 10, bold_first=True))
    b.append('<line x1="125" y1="38" x2="125" y2="200" stroke="#000" stroke-width="1" stroke-dasharray="3 3"/>')
    b.append('<line x1="535" y1="38" x2="535" y2="200" stroke="#000" stroke-width="1" stroke-dasharray="3 3"/>')
    b.append(arrow(125, 66, 535, 66)); b.append(text_bg(330, 60, t["s1"], 9.5))
    b.append(arrow(125, 98, 535, 98, style="dashed")); b.append(text_bg(330, 92, t["s2"], 9.5))
    b.append(arrow(535, 130, 125, 130, style="dashed")); b.append(text_bg(330, 124, t["s3"], 9.5))
    b.append(arrow(535, 162, 125, 162)); b.append(text_bg(330, 156, t["s4"], 9.5))
    b.append(text_bg(330, 190, t["s5"], 9.5, weight="bold"))
    b.append(text(330, 218, t["note"], 9, italic=True))
    return _svg(660, 226, "".join(b), t["title"])


# --------------------------------------------------------------------------
# Figure 5: tick-to-trade bars (two configurations, black/white patterns)

F5 = {
    "zh": dict(title="tick-to-trade p50（µs）", rows=["进程内：tick → 本帧第一条消息编码", "回环：tick → 第一条消息 send() 返回", "往返：tick → Accepted 解码"],
               sleep="交易所线程睡在 epoll_wait", busy="交易所线程忙轮询"),
    "en": dict(title="tick-to-trade p50 (µs)", rows=["in-process: tick → first message encoded", "loopback: tick → first message send() returned", "round trip: tick → Accepted decoded"],
               sleep="exchange thread sleeps in epoll_wait", busy="exchange thread busy-polls"),
}


def fig_t2t(lang: str, values) -> str:
    """values: [((sleep, label), (busy, label)), ...] in µs for the three rows."""
    t = F5[lang]
    b = []
    x0, scale, bar = 300, 11.0, 13
    for i, (row, ((a, la), (c, lc))) in enumerate(zip(t["rows"], values)):
        y = 20 + i * 46
        b.append(text(x0 - 8, y + 17, row, 9.5, anchor="end"))
        b.append(f'<rect x="{x0}" y="{y}" width="{max(a*scale,1.5):.1f}" height="{bar}" fill="#fff" stroke="#000" stroke-width="1.1"/>')
        b.append(text(x0 + max(a * scale, 1.5) + 4, y + 10, la, 9, anchor="start"))
        b.append(f'<rect x="{x0}" y="{y+bar+3}" width="{max(c*scale,1.5):.1f}" height="{bar}" fill="url(#hatch)" stroke="#000" stroke-width="1.1"/>')
        b.append(text(x0 + max(c * scale, 1.5) + 4, y + bar + 13, lc, 9, anchor="start"))
    b.append(f'<line x1="{x0}" y1="14" x2="{x0}" y2="150" stroke="#000" stroke-width="1"/>')
    y = 160
    b.append(f'<rect x="20" y="{y}" width="22" height="11" fill="#fff" stroke="#000"/>')
    b.append(text(48, y + 10, t["sleep"], 9, anchor="start"))
    b.append(f'<rect x="300" y="{y}" width="22" height="11" fill="url(#hatch)" stroke="#000"/>')
    b.append(text(328, y + 10, t["busy"], 9, anchor="start"))
    return _svg(660, 178, "".join(b), t["title"])
