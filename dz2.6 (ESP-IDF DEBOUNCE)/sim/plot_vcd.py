#!/usr/bin/env python3
"""Осцилограма з VCD у SVG — щоб у README були картинки, а не лише таблиці.

Малює два канали (кнопка і світлодіод) у заданому вікні часу. Без залежностей:
matplotlib тут не потрібен, а SVG GitHub показує як звичайну картинку.

    sim/plot_vcd.py raw_interrupt/wokwi.vcd --from 1.09 --to 1.13 \
        --out raw_interrupt/wave-burst.svg --title "Пачка брязкоту"
"""

import argparse
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from analyze_vcd import edges, parse_vcd  # noqa: E402

W, H = 920, 230
LEFT, RIGHT, TOP = 90, 20, 34
ROW_H, GAP = 56, 30
COLORS = {"button": "#2d7dd2", "led": "#d64550"}


def level_at(trace, t):
    value = trace[0][1] if trace else 0
    for time_ns, v in trace:
        if time_ns > t:
            break
        value = v
    return value


def polyline(trace, t0, t1, x0, x1, y_hi, y_lo):
    """Ступінчата лінія рівнів у межах вікна."""
    pts = []
    span = t1 - t0

    def x_of(t):
        return x0 + (t - t0) / span * (x1 - x0)

    def y_of(v):
        return y_hi if v else y_lo

    value = level_at(trace, t0)
    pts.append((x_of(t0), y_of(value)))
    for time_ns, v in trace:
        if time_ns <= t0 or time_ns > t1:
            continue
        pts.append((x_of(time_ns), y_of(value)))
        pts.append((x_of(time_ns), y_of(v)))
        value = v
    pts.append((x_of(t1), y_of(value)))
    return " ".join(f"{x:.2f},{y:.2f}" for x, y in pts)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("vcd")
    ap.add_argument("--from", dest="t_from", type=float, required=True, help="секунди")
    ap.add_argument("--to", dest="t_to", type=float, required=True, help="секунди")
    ap.add_argument("--out", required=True)
    ap.add_argument("--title", default="")
    ap.add_argument("--button", default=None)
    ap.add_argument("--led", default=None)
    args = ap.parse_args()

    _, series, order = parse_vcd(args.vcd)
    button = args.button or next((n for n in order if "BUTTON" in n.upper()), order[0])
    led = args.led or next((n for n in order if "LED" in n.upper()), order[1])
    t0, t1 = args.t_from * 1e9, args.t_to * 1e9

    rows = [("кнопка", edges(series[button]), COLORS["button"]),
            ("світлодіод", edges(series[led]), COLORS["led"])]

    x0, x1 = LEFT, W - RIGHT
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" '
           f'height="{H}" font-family="-apple-system,Segoe UI,Helvetica,sans-serif">',
           f'<rect width="{W}" height="{H}" fill="#fff"/>']
    if args.title:
        out.append(f'<text x="{LEFT}" y="20" font-size="14" fill="#111">{args.title}</text>')

    for i, (label, trace, color) in enumerate(rows):
        y_top = TOP + i * (ROW_H + GAP)
        y_hi, y_lo = y_top + 8, y_top + ROW_H - 8
        out.append(f'<text x="{LEFT - 10}" y="{(y_hi + y_lo) / 2 + 4:.0f}" font-size="12" '
                   f'text-anchor="end" fill="#555">{label}</text>')
        out.append(f'<line x1="{x0}" y1="{y_lo}" x2="{x1}" y2="{y_lo}" stroke="#eee"/>')
        out.append(f'<polyline fill="none" stroke="{color}" stroke-width="1.6" '
                   f'points="{polyline(trace, t0, t1, x0, x1, y_hi, y_lo)}"/>')

    span_ms = (t1 - t0) / 1e6
    for k in range(5):
        x = x0 + k / 4 * (x1 - x0)
        ms = args.t_from * 1000 + k / 4 * span_ms
        out.append(f'<line x1="{x:.1f}" y1="{TOP}" x2="{x:.1f}" y2="{H - 24}" '
                   f'stroke="#f0f0f0"/>')
        out.append(f'<text x="{x:.1f}" y="{H - 8}" font-size="11" fill="#777" '
                   f'text-anchor="middle">{ms:.1f} мс</text>')

    out.append("</svg>")
    with open(args.out, "w") as fp:
        fp.write("\n".join(out) + "\n")
    print(f"{args.out}: вікно {span_ms:.2f} мс")
    return 0


if __name__ == "__main__":
    sys.exit(main())
