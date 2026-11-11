"""Draw the README charts (SVG) from the JSON files that bench.py writes.

  python tools/plot_results.py            # uses the files in docs/results

Writes into docs/images:
  1g-throughput.svg    single core vs dual core (flow control on)
  1g-udp-rx-sweep.svg  UDP RX received rate vs offered rate, single vs dual core
  1g-flowctrl.svg      single core, flow control off vs on
Plain SVG with its own light background, so it reads the same in GitHub's light and dark themes.
"""
import json
import os

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
RESULTS = os.path.join(ROOT, "docs", "results")
IMAGES = os.path.join(ROOT, "docs", "images")

SURFACE = "#fcfcfb"
TEXT = "#0b0b0b"
TEXT2 = "#52514e"
MUTED = "#898781"
GRID = "#e4e3df"
BLUE, ORANGE = "#2a78d6", "#eb6834"  # categorical slots 1, 2
FONT = "'Noto Sans KR','Malgun Gothic','Apple SD Gothic Neo',sans-serif"


def load(name):
    with open(os.path.join(RESULTS, name), encoding="utf-8") as f:
        return json.load(f)


def lossless_max(points, limit=0.1):
    ok = [p["offered_mbps"] for p in points if p["loss_pct"] <= limit]
    return max(ok) if ok else 0


METRICS = [
    (lambda r: round(r["tcp_rx"]["board_mbps"]["mean"], 1), "{:.1f}"),
    (lambda r: round(r["tcp_tx"]["board_mbps"]["mean"], 1), "{:.1f}"),
    (lambda r: round(r["udp_tx"]["board_mbps"]["mean"], 1), "{:.1f}"),
    (lambda r: lossless_max(r["udp_rx_sweep"]), "{:.0f}"),
]

# All chart text, per language. Korean charts are *.svg, English charts are *.en.svg.
TEXT_KO = {
    "suffix": "",
    "rows": ["TCP 수신 (PC → 보드)", "TCP 송신 (보드 → PC)", "UDP 송신 (보드 → PC)", "UDP 수신 (손실 없는 최대)"],
    "subtitle": ["보드 iperf 측정, Mbit/s. TCP·UDP는 10초 측정 5회의 평균,",
                 "UDP 수신은 손실 0.1 % 이하로 받은 최대 송신 속도"],
    "cores": ["단일 코어", "듀얼 코어"],
    "flow": ["흐름 제어 꺼짐", "흐름 제어 켬"],
    "title_cores": "1G 처리량: 단일 코어와 듀얼 코어",
    "title_flow": "흐름 제어 효과 (단일 코어)",
    "title_sweep": "1G UDP 수신: 보낸 속도별 보드 수신 속도",
    "sweep_sub": "PC가 5초 동안 일정 속도로 보낼 때 보드가 받은 속도, Mbit/s",
    "sweep_x": "PC 송신 속도 (Mbit/s)",
    "ideal": "손실 없음",
    "peak": "최대",
}
TEXT_EN = {
    "suffix": ".en",
    "rows": ["TCP receive (PC → board)", "TCP transmit (board → PC)", "UDP transmit (board → PC)",
             "UDP receive (max without loss)"],
    "subtitle": ["Board iperf, Mbit/s. TCP and UDP: mean of five 10 s runs.",
                 "UDP receive: highest offered rate received with ≤ 0.1 % loss."],
    "cores": ["Single core", "Dual core"],
    "flow": ["Flow control off", "Flow control on"],
    "title_cores": "1G throughput: single core vs dual core",
    "title_flow": "Effect of flow control (single core)",
    "title_sweep": "1G UDP receive: board rate vs offered rate",
    "sweep_sub": "Rate the board received while the PC sent at a fixed rate for 5 s, Mbit/s",
    "sweep_x": "PC send rate (Mbit/s)",
    "ideal": "no loss",
    "peak": "max",
}


def svg(w, h, body):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}" '
            f'font-family="{FONT}">\n<rect width="{w}" height="{h}" rx="8" fill="{SURFACE}"/>\n{body}</svg>\n')


def header(title, subtitle):
    out = [f'<text x="24" y="34" font-size="17" font-weight="600" fill="{TEXT}">{title}</text>\n']
    for i, line in enumerate(subtitle):
        out.append(f'<text x="24" y="{56 + 18 * i}" font-size="12.5" fill="{TEXT2}">{line}</text>\n')
    return "".join(out)


def text_width(s, size=12):
    """Rough width: Hangul is about one em wide, Latin about half."""
    return sum(size if ord(c) > 0x2E7F else size * 0.56 for c in s)


def legend(x, y, labels, colors):
    out = []
    for lab, color in zip(labels, colors):
        out.append(f'<rect x="{x}" y="{y - 10}" width="12" height="12" rx="3" fill="{color}"/>\n')
        out.append(f'<text x="{x + 18}" y="{y}" font-size="12" fill="{TEXT}">{lab}</text>\n')
        x += 18 + text_width(lab) + 28
    return "".join(out)


def bar_end(x0, y, length, h, color):
    """Horizontal bar anchored at x0 with a 4px rounded data end."""
    r = min(4, length / 2, h / 2)
    return (f'<path d="M{x0},{y} h{length - r:.1f} a{r},{r} 0 0 1 {r},{r} v{h - 2 * r} '
            f'a{r},{r} 0 0 1 {-r},{r} h{-(length - r):.1f} z" fill="{color}"/>\n')


def throughput_chart(runs, labels, colors, title, path, t, vmax=300):
    rows = [(name, get, fmt) for name, (get, fmt) in zip(t["rows"], METRICS)]
    w, right = 760, 70
    left = int(max(text_width(n, 12.5) for n, _, _ in rows)) + 40
    top = 56 + 18 * len(t["subtitle"]) + 30
    bar_h, gap, group_gap = 18, 2, 26
    group_h = len(runs) * bar_h + (len(runs) - 1) * gap
    h = top + len(rows) * group_h + (len(rows) - 1) * group_gap + 56
    scale = (w - left - right) / vmax
    b = [header(title, t["subtitle"]), legend(24, top - 20, labels, colors)]
    plot_bottom = h - 40
    for v in range(0, vmax + 1, 50):
        x = left + v * scale
        b.append(f'<line x1="{x:.1f}" y1="{top - 6}" x2="{x:.1f}" y2="{plot_bottom}" stroke="{GRID}" stroke-width="1"/>\n')
        b.append(f'<text x="{x:.1f}" y="{plot_bottom + 16}" font-size="11" fill="{MUTED}" text-anchor="middle">{v}</text>\n')
    b.append(f'<text x="{w - right:.1f}" y="{plot_bottom + 32}" font-size="11" fill="{MUTED}" text-anchor="end">Mbit/s</text>\n')
    y = top
    for name, get, fmt in rows:
        b.append(f'<text x="{left - 12}" y="{y + group_h / 2 + 4:.1f}" font-size="12.5" fill="{TEXT}" text-anchor="end">{name}</text>\n')
        for i, r in enumerate(runs):
            val = get(r)
            yy = y + i * (bar_h + gap)
            b.append(bar_end(left, yy, max(val * scale, 1), bar_h, colors[i]))
            b.append(f'<text x="{left + val * scale + 6:.1f}" y="{yy + bar_h - 5}" font-size="11.5" fill="{TEXT2}">{fmt.format(val)}</text>\n')
        y += group_h + group_gap
    with open(path, "w", encoding="utf-8") as f:
        f.write(svg(w, h, "".join(b)))


def sweep_chart(runs, labels, colors, title, path, t):
    w, h = 760, 420
    left, right, top, bottom = 64, 90, 104, 56
    xs = sorted({p["offered_mbps"] for r in runs for p in r["udp_rx_sweep"]})
    xmin, xmax, ymax = xs[0], xs[-1], 250
    px = lambda v: left + (v - xmin) / (xmax - xmin) * (w - left - right)
    py = lambda v: top + (1 - v / ymax) * (h - top - bottom)
    b = [header(title, [t["sweep_sub"]])]
    peaks = [max(p["received_mbps"] for p in r["udp_rx_sweep"]) for r in runs]
    b.append(legend(24, 84, [f"{lab} ({t['peak']} {pk:g})" for lab, pk in zip(labels, peaks)], colors))
    for v in range(0, ymax + 1, 50):
        b.append(f'<line x1="{left}" y1="{py(v):.1f}" x2="{w - right}" y2="{py(v):.1f}" stroke="{GRID}" stroke-width="1"/>\n')
        b.append(f'<text x="{left - 8}" y="{py(v) + 4:.1f}" font-size="11" fill="{MUTED}" text-anchor="end">{v}</text>\n')
    for v in xs:
        b.append(f'<text x="{px(v):.1f}" y="{h - bottom + 18}" font-size="11" fill="{MUTED}" text-anchor="middle">{v:g}</text>\n')
    b.append(f'<text x="{(left + w - right) / 2:.1f}" y="{h - 14}" font-size="12" fill="{TEXT2}" text-anchor="middle">{t["sweep_x"]}</text>\n')
    # ideal line: everything offered is received
    b.append(f'<line x1="{px(xmin):.1f}" y1="{py(xmin):.1f}" x2="{px(ymax):.1f}" y2="{py(ymax):.1f}" '
             f'stroke="{MUTED}" stroke-width="1.5" stroke-dasharray="5 4"/>\n')
    b.append(f'<text x="{px(ymax) + 6:.1f}" y="{py(ymax) + 4:.1f}" font-size="11" fill="{MUTED}">{t["ideal"]}</text>\n')
    for i, r in enumerate(runs):
        pts = [(p["offered_mbps"], p["received_mbps"]) for p in sorted(r["udp_rx_sweep"], key=lambda p: p["offered_mbps"])]
        d = " ".join(f'{"M" if k == 0 else "L"}{px(x):.1f},{py(y):.1f}' for k, (x, y) in enumerate(pts))
        b.append(f'<path d="{d}" fill="none" stroke="{colors[i]}" stroke-width="2" stroke-linejoin="round"/>\n')
        for x, y in pts:
            b.append(f'<circle cx="{px(x):.1f}" cy="{py(y):.1f}" r="4" fill="{colors[i]}" stroke="{SURFACE}" stroke-width="2"/>\n')
    with open(path, "w", encoding="utf-8") as f:
        f.write(svg(w, h, "".join(b)))


def main():
    single, dual = load("1g-single-core.json"), load("1g-dual-core.json")
    no_fc = os.path.join(RESULTS, "1g-single-core-no-flowctrl.json")
    for t in (TEXT_KO, TEXT_EN):
        out = lambda name: os.path.join(IMAGES, f"{name}{t['suffix']}.svg")
        throughput_chart([single, dual], t["cores"], [BLUE, ORANGE], t["title_cores"], out("1g-throughput"), t)
        sweep_chart([single, dual], t["cores"], [BLUE, ORANGE], t["title_sweep"], out("1g-udp-rx-sweep"), t)
        if os.path.exists(no_fc):
            throughput_chart([load("1g-single-core-no-flowctrl.json"), single], t["flow"], [MUTED, BLUE],
                             t["title_flow"], out("1g-flowctrl"), t)
    print("wrote", os.path.normpath(IMAGES))


if __name__ == "__main__":
    main()
