#!/usr/bin/env python3
"""Розбір VCD з логічного аналізатора Wokwi для ДЗ 2.6.

Рахує те саме, що я рахував по CSV із Logic 2 у ДЗ 2.4: скільком натисканням
відповідає скільки реакцій світлодіода, скільки зайвих подій і яка затримка.

    sim/analyze_vcd.py raw_interrupt/wokwi.vcd
    sim/analyze_vcd.py --channels dz2.5\\ \\(TIMERS\\)/timer_tick/wokwi.vcd
"""

import argparse
import statistics
import sys

UNITS = {"s": 1e9, "ms": 1e6, "us": 1e3, "ns": 1.0, "ps": 1e-3}
IDLE_GAP_MS = 80  # пауза, після якої спад вважається новим натисканням


def parse_vcd(path):
    """-> (scale_ns, {name: [(time_ns, value)]}, [names в порядку оголошення])"""
    names, series, scale = {}, {}, 1.0
    order = []
    time_ns = 0.0
    with open(path) as fp:
        for raw in fp:
            line = raw.strip()
            if not line:
                continue
            if line.startswith("$timescale"):
                spec = line.split()[1] if len(line.split()) > 1 else "1ns"
                num = "".join(c for c in spec if c.isdigit()) or "1"
                unit = "".join(c for c in spec if c.isalpha()) or "ns"
                scale = float(num) * UNITS[unit]
            elif line.startswith("$var"):
                parts = line.split()
                ident, name = parts[3], parts[4]
                names[ident] = name
                series[name] = []
                order.append(name)
            elif line.startswith("#"):
                time_ns = float(line[1:]) * scale
            elif line and line[0] in "01" and len(line) > 1:
                ident = line[1:]
                if ident in names:
                    series[names[ident]].append((time_ns, int(line[0])))
    return scale, series, order


def edges(samples):
    """Переходи, без повторів того самого рівня."""
    out, prev = [], None
    for t, v in samples:
        if v != prev:
            out.append((t, v))
            prev = v
    return out


def bursts(button_edges, idle_ns):
    """Групи фронтів: нове натискання — спад після idle_ns тишi у HIGH."""
    groups, current, last_high = [], [], None
    for t, v in button_edges:
        if v == 0:  # спад
            if current and last_high is not None and t - last_high >= idle_ns:
                groups.append(current)
                current = []
            current.append((t, v))
        else:
            last_high = t
            if current:
                current.append((t, v))
    if current:
        groups.append(current)
    return groups


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("vcd")
    ap.add_argument("--channels", action="store_true", help="лише перелік каналів")
    ap.add_argument("--button", help="назва канала кнопки")
    ap.add_argument("--led", help="назва канала світлодіода")
    ap.add_argument("--markdown", action="store_true", help="таблиця для README")
    ap.add_argument("--csv", help="експорт фронтів у CSV формату Logic 2")
    args = ap.parse_args()

    _, series, order = parse_vcd(args.vcd)
    if args.channels or len(order) < 2:
        for name in order:
            print(f"{name}: {len(edges(series[name]))} переходів")
        return 0

    button = args.button or next((n for n in order if "BUTTON" in n.upper()), order[0])
    led = args.led or next((n for n in order if "LED" in n.upper()), order[1])

    # Рівні, записані на #0, — початковий стан лінії, а не подія.
    b_edges = [e for e in edges(series[button]) if e[0] > 0]
    # Відкидаємо лише рівень, записаний на #0: це початковий стан, а не подія.
    l_edges = [t for t, _ in edges(series[led]) if t > 0]
    groups = bursts(b_edges, IDLE_GAP_MS * 1e6)

    rows, latencies, no_reaction = [], [], 0
    used = 0
    for i, group in enumerate(groups, 1):
        start = group[0][0]
        end = group[-1][0]
        reacted = [t for t in l_edges if start <= t <= end + IDLE_GAP_MS * 1e6]
        used += len(reacted)
        if reacted:
            lat_ms = (reacted[0] - start) / 1e6
            latencies.append(lat_ms)
        else:
            lat_ms = None
            no_reaction += 1
        rows.append((i, start / 1e9, len(group), len(reacted), lat_ms))

    extra = len(l_edges) - used
    print(f"файл: {args.vcd}")
    print(f"канали: кнопка={button}, світлодіод={led}")
    print(f"натискань на лінії: {len(groups)}")
    print(f"фронтів на кнопці: {len(b_edges)}")
    print(f"перемикань світлодіода: {len(l_edges)}")
    print(f"натискань без реакції: {no_reaction}")
    print(f"реакцій поза натисканнями: {extra}")
    if latencies:
        print(f"затримка, мс: мін {min(latencies):.3f} / медіана "
              f"{statistics.median(latencies):.3f} / макс {max(latencies):.3f}")

    if args.csv:
        # Той самий формат, що експорт із Logic 2 у ДЗ 2.4: рядок на кожну зміну.
        merged = sorted([(t, 0, v) for t, v in edges(series[button])] +
                        [(t, 1, v) for t, v in edges(series[led])])
        level = [1, 0]
        with open(args.csv, "w") as fp:
            fp.write("Time [s],Channel 0,Channel 1\n")
            for t, ch, v in merged:
                level[ch] = v
                fp.write(f"{t / 1e9:.9f},{level[0]},{level[1]}\n")
        print(f"CSV: {args.csv} ({len(merged)} рядків)")

    if args.markdown:
        print("\n| № | час, с | фронтів | реакцій | затримка, мс |")
        print("|---:|---:|---:|---:|---:|")
        for i, t, ne, nr, lat in rows:
            print(f"| {i} | {t:.3f} | {ne} | {nr} | "
                  f"{'—' if lat is None else f'{lat:.3f}'} |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
