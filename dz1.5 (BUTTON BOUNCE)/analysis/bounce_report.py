#!/usr/bin/env python3
"""Звіт про брязкіт кнопки із запису логічного аналізатора (ДЗ 1.5).

Читає запис лінії кнопки (VCD або CSV), ділить фронти на пачки брязкоту,
для кожного натискання рахує імпульси (FALLING-фронти) і тривалість брязкоту,
порівнює з логом прошивки ../counter і пише markdown-таблицю.

Використання (з папки "dz1.5 (BUTTON BOUNCE)"):
  python3 analysis/bounce_report.py analysis/24msps-2/capture.csv
  python3 analysis/bounce_report.py analysis/24msps-2/capture.csv --mcu-log analysis/24msps-2/serial.log \\
      --plots analysis/24msps-2/plots --update-readme analysis/README.md --section 24msps-2

Формати запису:
  .csv  Saleae Logic 2: Export Data → CSV (рядки "час_у_секундах,рівень" з заголовком)
  .vcd  PulseView: Export → Value Change Dump
"""
from __future__ import annotations

import argparse
import csv
import math
import os
import re
import statistics
import sys
from dataclasses import dataclass, field
from pathlib import Path

TIME_UNITS = {"s": 1.0, "ms": 1e-3, "us": 1e-6, "µs": 1e-6, "ns": 1e-9, "ps": 1e-12, "fs": 1e-15}
# counter: "PRESS #1 FALLING: 17 ...", debounce: "PRESS #1 edges: 95 ignored: 94 ..."
MCU_LINE = re.compile(r"\b(PRESS|RELEASE|TAP|HOLD)\s+#\s*(\d+)\s+(FALLING|edges):\s*(\d+)")


class ReportError(Exception):
    pass


# ---------------------------------------------------------------- loading

@dataclass
class Signal:
    name: str
    initial: int
    edges: list[tuple[float, int]]  # (час, с; рівень після фронту)
    resolution: float | None        # крок часу у файлі, с


def iter_tokens(path: Path):
    with path.open(encoding="utf-8", errors="replace") as f:
        for line in f:
            yield from line.split()


def take_until_end(tokens) -> list[str]:
    parts = []
    for tok in tokens:
        if tok == "$end":
            return parts
        parts.append(tok)
    raise ReportError("VCD обривається всередині $-блоку без $end")


def pick_by_name(names: list[str], channel: str | None, what: str) -> int:
    if not names:
        raise ReportError(f"у файлі немає жодного {what}")
    if channel is None:
        return 0
    if channel.isdigit() and int(channel) < len(names) and channel not in names:
        return int(channel)
    folded = [n.casefold() for n in names]
    wanted = channel.casefold()
    if wanted in folded:
        return folded.index(wanted)
    partial = [i for i, n in enumerate(folded) if wanted in n]
    if len(partial) == 1:
        return partial[0]
    raise ReportError(f"канал '{channel}' не знайдено, є: {', '.join(names)}")


def load_vcd(path: Path, channel: str | None) -> Signal:
    tokens = iter_tokens(path)
    timescale = 1e-9
    wires: list[tuple[str, str]] = []  # (ідентифікатор, назва) лише 1-бітних змінних

    for tok in tokens:
        if tok == "$timescale":
            spec = "".join(take_until_end(tokens))
            m = re.fullmatch(r"(\d+)(\w+)", spec)
            if not m or m[2] not in TIME_UNITS:
                raise ReportError(f"невідомий $timescale: {spec}")
            timescale = int(m[1]) * TIME_UNITS[m[2]]
        elif tok == "$var":
            parts = take_until_end(tokens)  # тип розмір ідентифікатор назва [діапазон]
            if len(parts) >= 4 and parts[1] == "1":
                wires.append((parts[2], " ".join(parts[3:])))
        elif tok == "$enddefinitions":
            take_until_end(tokens)
            break
        elif tok.startswith("$") and tok != "$end":
            take_until_end(tokens)

    index = pick_by_name([name for _, name in wires], channel, "1-бітного сигналу")
    wire_id, name = wires[index]

    t = 0.0
    level: int | None = None
    initial = 1
    edges: list[tuple[float, int]] = []
    for tok in tokens:
        head = tok[0]
        if head == "#" and tok[1:].isdigit():
            t = int(tok[1:]) * timescale
            continue
        if head == "$":
            if tok == "$comment":
                take_until_end(tokens)
            continue
        if head in "01xXzZ":
            ident, value = tok[1:], head
        elif head in "bB":
            ident, value = next(tokens, ""), tok[-1]
        elif head in "rR":
            next(tokens, "")
            continue
        else:
            continue
        if ident != wire_id or value not in "01":
            continue
        v = int(value)
        if level is None:
            initial = level = v
        elif v != level:
            edges.append((t, v))
            level = v

    return Signal(name, initial, edges, timescale)


def load_csv(path: Path, channel: str | None) -> Signal:
    header: list[str] | None = None
    column: int | None = None
    level: int | None = None
    initial = 1
    edges: list[tuple[float, int]] = []

    with path.open(newline="", encoding="utf-8-sig") as f:
        lines = (line for line in f if line.strip() and not line.lstrip().startswith((";", "#")))
        for row in csv.reader(lines):
            try:
                t = float(row[0])
            except ValueError:
                if header is None:
                    header = [h.strip() for h in row]
                continue
            if column is None:
                names = header[1:] if header else [str(i) for i in range(1, len(row))]
                column = 1 + pick_by_name(names, channel, "стовпця з рівнем")
            v = 1 if float(row[column]) >= 0.5 else 0
            if level is None:
                initial = level = v
            elif v != level:
                edges.append((t, v))
                level = v

    if column is None:
        raise ReportError("у CSV немає рядків з даними")
    name = header[column] if header else f"стовпець {column}"
    return Signal(name, initial, edges, None)


# ---------------------------------------------------------------- analysis

@dataclass
class Burst:
    edges: list[tuple[float, int]]

    @property
    def before(self) -> int:
        return 1 - self.edges[0][1]

    @property
    def after(self) -> int:
        return self.edges[-1][1]

    @property
    def start(self) -> float:
        return self.edges[0][0]

    @property
    def end(self) -> float:
        return self.edges[-1][0]

    @property
    def duration(self) -> float:
        return self.end - self.start

    @property
    def falling(self) -> int:
        return sum(1 for _, v in self.edges if v == 0)

    @property
    def max_gap(self) -> float:
        return max((b[0] - a[0] for a, b in zip(self.edges, self.edges[1:])), default=0.0)

    @property
    def kind(self) -> str:
        return {(1, 0): "PRESS", (0, 1): "RELEASE", (1, 1): "TAP", (0, 0): "HOLD"}[(self.before, self.after)]


@dataclass
class Press:
    press: Burst
    release: Burst | None = None
    hold: list[Burst] = field(default_factory=list)
    mcu: dict | None = None

    @property
    def is_tap(self) -> bool:
        return self.press.kind == "TAP"

    @property
    def falling_total(self) -> int:
        return self.press.falling + (self.release.falling if self.release else 0) + sum(b.falling for b in self.hold)

    @property
    def edges_total(self) -> int:
        return len(self.press.edges) + (len(self.release.edges) if self.release else 0) + sum(len(b.edges) for b in self.hold)

    def la_total(self, unit: str) -> int:
        return self.edges_total if unit == "edges" else self.falling_total

    @property
    def hold_time(self) -> float | None:
        return self.release.start - self.press.end if self.release else None


def split_bursts(signal: Signal, gap: float) -> list[Burst]:
    bursts: list[Burst] = []
    current: list[tuple[float, int]] = []
    for edge in signal.edges:
        if current and edge[0] - current[-1][0] > gap:
            bursts.append(Burst(current))
            current = []
        current.append(edge)
    if current:
        bursts.append(Burst(current))
    return bursts


def group_presses(bursts: list[Burst]) -> tuple[list[Press], int]:
    presses: list[Press] = []
    open_press: Press | None = None
    skipped = 0
    for burst in bursts:
        kind = burst.kind
        if kind in ("PRESS", "TAP"):
            presses.append(Press(burst))
            open_press = presses[-1] if kind == "PRESS" else None
        elif open_press is None:
            skipped += 1  # запис почався, коли кнопка вже була натиснута
        elif kind == "HOLD":
            open_press.hold.append(burst)
        else:
            open_press.release = burst
            open_press = None
    return presses, skipped


def load_mcu_log(path: Path) -> tuple[list[dict], str]:
    """Повертає натискання з логу і що в ньому пораховано: "falling" (counter) або "edges" (debounce)."""
    presses: list[dict] = []
    by_number: dict[int, dict] = {}
    unit = "falling"
    with path.open(encoding="utf-8", errors="replace") as f:
        for line in f:
            m = MCU_LINE.search(line)
            if not m:
                continue
            kind, number, count = m[1], int(m[2]), int(m[4])
            unit = "edges" if m[3] == "edges" else "falling"
            if kind in ("PRESS", "TAP"):
                record = {"press": count, "release": 0, "hold": 0, "tap": kind == "TAP"}
                presses.append(record)
                by_number[number] = record  # після перезавантаження МК номери знову з 1
            elif number in by_number:
                by_number[number][kind.lower()] += count
    return presses, unit


def mcu_total(record: dict) -> int:
    return record["press"] + record["release"] + record["hold"]


# ---------------------------------------------------------------- report

def plural(n: int, one: str, few: str, many: str) -> str:
    if n % 10 == 1 and n % 100 != 11:
        return f"{n} {one}"
    if 2 <= n % 10 <= 4 and not 12 <= n % 100 <= 14:
        return f"{n} {few}"
    return f"{n} {many}"


def ms(seconds: float | None, digits: int = 3) -> str:
    return "—" if seconds is None else f"{seconds * 1000:.{digits}f}"


def pulses(burst: Burst | None) -> str:
    if burst is None:
        return "—"
    return f"{burst.falling} ({len(burst.edges)})"


def stats_row(values: list[float]) -> str:
    if not values:
        return "—"
    return f"{ms(statistics.mean(values))} / {ms(statistics.median(values))} / {ms(max(values))}"


def recommended_ms(seconds: float) -> int:
    return max(1, math.ceil(seconds * 1000 * 2))


def build_report(signal: Signal, source: str, presses: list[Press], skipped: int, gap: float,
                 mcu_source: str | None, mcu_count: int | None, unit: str = "falling",
                 plots_src: str | None = None) -> str:
    has_mcu = mcu_source is not None
    what = "фронтів" if unit == "edges" else "імп."
    out: list[str] = []

    resolution = f", крок часу у файлі {signal.resolution * 1e9:g} нс" if signal.resolution else ""
    out.append(f"Запис: `{source}`, канал `{signal.name}`{resolution}. "
               f"Пачки брязкоту розділяються тишею > {gap * 1000:g} мс.")
    if has_mcu:
        counted = "всі фронти через CHANGE" if unit == "edges" else "FALLING-переривання"
        out.append(f"Лог мікроконтролера: `{mcu_source}` ({mcu_count} натискань, МК рахує {counted}).")
    out.append("")

    header = ("| № | Натискання: імп. (фронтів) | Брязкіт натискання, мс | Утримання, мс | Відпускання: імп. (фронтів) "
              f"| Брязкіт відпускання, мс | ЛА: {what} разом |")
    divider = "|---:|:---:|---:|---:|:---:|---:|:---:|"
    if has_mcu:
        header += f" МК: {what} разом | МК − ЛА |"
        divider += ":---:|:---:|"
    out += [header, divider]

    for i, p in enumerate(presses, 1):
        number = f"{i}" + (" (tap)" if p.is_tap else "")
        press_pulses = pulses(p.press) + (f" +{sum(b.falling for b in p.hold)} hold" if p.hold else "")
        row = (f"| {number} | {press_pulses} | {ms(p.press.duration)} | {ms(p.hold_time, 0)} | "
               f"{pulses(p.release)} | {ms(p.release.duration) if p.release else '—'} | {p.la_total(unit)} |")
        if has_mcu:
            if p.mcu is None:
                row += " — | — |"
            else:
                delta = mcu_total(p.mcu) - p.la_total(unit)
                row += f" {mcu_total(p.mcu)} | {'0' if delta == 0 else f'{delta:+d}'} |"
        out.append(row)

    out += ["", "_імп._ = FALLING-фронти (саме їх рахує переривання `FALLING`), у дужках усі фронти, "
            "_брязкіт_ = час від першого до останнього фронту пачки."]
    if skipped:
        out.append(f"Пропущено пачок на початку запису: {skipped} (лінія була в LOW: старт прошивки або натиснута кнопка).")

    press_bursts = [p.press for p in presses if not p.is_tap]
    release_bursts = [p.release for p in presses if p.release]
    all_bursts = press_bursts + release_bursts + [b for p in presses for b in p.hold]

    def falling_stats(bursts: list[Burst]) -> str:
        values = [b.falling for b in bursts]
        return f"{statistics.mean(values):.1f} / {max(values)}" if values else "—"

    out += ["", f"**Підсумок ({len(presses)} натискань)**", "",
            "| Показник | Натискання | Відпускання |",
            "|---|:---:|:---:|",
            f"| Імпульсів (FALLING): середнє / макс | {falling_stats(press_bursts)} | {falling_stats(release_bursts)} |",
            f"| Брязкіт, мс: середнє / медіана / макс | {stats_row([b.duration for b in press_bursts])} | "
            f"{stats_row([b.duration for b in release_bursts])} |",
            f"| Найдовша пауза між фронтами в пачці, мс | {ms(max((b.max_gap for b in press_bursts), default=None))} | "
            f"{ms(max((b.max_gap for b in release_bursts), default=None))} |",
            ""]

    if has_mcu:
        compared = [p for p in presses if p.mcu is not None]
        deltas = [mcu_total(p.mcu) - p.la_total(unit) for p in compared]
        mcu_sum, la_sum = sum(mcu_total(p.mcu) for p in compared), sum(p.la_total(unit) for p in compared)
        if unit == "edges":
            ok = "кожне натискання зараховане один раз" if mcu_count == len(presses) else "є зайві або пропущені натискання"
            out.append(f"- Фільтр МК зарахував **{mcu_count}** натискань, на записі аналізатора **{len(presses)}**: {ok}.")
        out.append(f"- Лічильник МК збігся з аналізатором у **{deltas.count(0)} з {len(compared)}** натискань; "
                   f"МК порахував менше: {sum(d < 0 for d in deltas)}, більше: {sum(d > 0 for d in deltas)}. "
                   f"Разом МК {mcu_sum} із {la_sum} {what} ({mcu_sum / la_sum * 100:.0f} %).")
        if mcu_count != len(presses):
            out.append(f"- ⚠️ Кількість натискань різна: аналізатор {len(presses)}, МК {mcu_count}. "
                       "Порівняння йде по порядку, перевір `--mcu-skip`.")

    if all_bursts:
        longest = max(b.duration for b in all_bursts)
        widest_gap = max(b.max_gap for b in all_bursts)
        out += [f"- Найдовший брязкіт: **{ms(longest)} мс**. Debounce «ігнорувати фронти N мс після першого» "
                f"має бути не менше за нього, із запасом ×2: **{recommended_ms(longest)} мс**.",
                f"- Найдовша пауза всередині брязкоту: **{ms(widest_gap)} мс**. Debounce «чекати N мс тиші» "
                f"(варіант `debounce`) має бути більшим за неї, із запасом ×2: **{recommended_ms(widest_gap)} мс**."]

    if plots_src:
        out += plots_gallery(presses, plots_src)

    return "\n".join(out) + "\n"


def plots_gallery(presses: list[Press], src: str) -> list[str]:
    lines = ["", "<details open>",
             f"<summary><b>Графіки: {plural(len(presses), 'натискання', 'натискання', 'натискань')}</b>, "
             "ліворуч натискання, праворуч відпускання</summary>", ""]
    for i, p in enumerate(presses, 1):
        release = f"{p.release.falling} імп. відпускання" if p.release else "відпускання не записано"
        lines.append(f'<img src="{src}/press-{i:02d}.svg" width="100%" alt="Натискання {i}: '
                     f'{p.press.falling} імп. натискання, {release}">')
    lines += ["", "</details>"]
    return lines


# ---------------------------------------------------------------- plots

def nice_step(span_ms: float, max_ticks: int = 6) -> float:
    for base in (0.001, 0.01, 0.1, 1, 10, 100, 1000):
        for mult in (1, 2, 5):
            if span_ms / (base * mult) <= max_ticks:
                return base * mult
    return span_ms


def svg_panel(burst: Burst | None, x0: float, width: float, title: str) -> list[str]:
    top, high, low = 30, 45, 105
    parts = [f'<text x="{x0}" y="18" class="title">{title}</text>',
             f'<text x="{x0 - 8}" y="{high + 4}" class="axis" text-anchor="end">1</text>',
             f'<text x="{x0 - 8}" y="{low + 4}" class="axis" text-anchor="end">0</text>']
    if burst is None:
        parts.append(f'<text x="{x0}" y="{(high + low) / 2}" class="axis">не записано</text>')
        return parts

    pad = max(burst.duration * 0.25, 0.00001)
    t0, t1 = burst.start - pad, burst.end + pad

    def x(t: float) -> float:
        return x0 + (t - t0) / (t1 - t0) * width

    def y(level: int) -> int:
        return high if level else low

    points = [f"{x(t0):.2f},{y(burst.before)}"]
    level = burst.before
    for t, v in burst.edges:
        points.append(f"{x(t):.2f},{y(level)}")
        points.append(f"{x(t):.2f},{y(v)}")
        level = v
    points.append(f"{x(t1):.2f},{y(level)}")

    parts.append(f'<line x1="{x0}" y1="{low + 12}" x2="{x0 + width}" y2="{low + 12}" class="grid"/>')
    step = nice_step((t1 - t0) * 1000)
    tick = math.ceil((t0 - burst.start) * 1000 / step) * step
    while tick <= (t1 - burst.start) * 1000 + 1e-9:
        tx = x(burst.start + tick / 1000)
        parts.append(f'<line x1="{tx:.2f}" y1="{top + 8}" x2="{tx:.2f}" y2="{low + 12}" class="grid"/>')
        parts.append(f'<text x="{tx:.2f}" y="{low + 26}" class="axis" text-anchor="middle">{tick:g}</text>')
        tick += step
    parts.append(f'<polyline points="{" ".join(points)}" class="wave"/>')
    return parts


def write_plots(presses: list[Press], directory: Path) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    width, height, panel = 900, 150, 400
    for i, p in enumerate(presses, 1):
        hold = f", +{sum(b.falling for b in p.hold)} hold" if p.hold else ""
        press_title = (f"#{i} {'tap' if p.is_tap else 'натискання'}: {p.press.falling} імп. "
                       f"({plural(len(p.press.edges), 'фронт', 'фронти', 'фронтів')}{hold}), {ms(p.press.duration)} мс")
        release_title = (f"відпускання: {p.release.falling} імп. ({plural(len(p.release.edges), 'фронт', 'фронти', 'фронтів')}), "
                         f"{ms(p.release.duration)} мс" if p.release else "відпускання")
        body = svg_panel(p.press, 30, panel, press_title) + svg_panel(p.release, 470, panel, release_title)
        svg = "\n".join([
            f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
            "<style>.title{font:600 13px sans-serif;fill:#1f2328}.axis{font:11px sans-serif;fill:#59636e}"
            ".grid{stroke:#d1d9e0;stroke-width:1}.wave{fill:none;stroke:#0969da;stroke-width:1.5}</style>",
            f'<rect width="{width}" height="{height}" fill="#ffffff"/>',
            *body,
            f'<text x="{width - 12}" y="{height - 6}" class="axis" text-anchor="end">мс від першого фронту</text>',
            "</svg>",
        ])
        (directory / f"press-{i:02d}.svg").write_text(svg + "\n", encoding="utf-8")


def update_readme(path: Path, report: str, section: str | None) -> None:
    name = f"bounce-report:{section}" if section else "bounce-report"
    start_marker, end_marker = f"<!-- {name}:start -->", f"<!-- {name}:end -->"
    text = path.read_text(encoding="utf-8")
    start, end = text.find(start_marker), text.find(end_marker)
    if start < 0 or end < start:
        raise ReportError(f"у {path} немає маркерів {start_marker} … {end_marker}")
    new_text = text[:start + len(start_marker)] + "\n" + report + text[end:]
    path.write_text(new_text, encoding="utf-8")


# ---------------------------------------------------------------- main

def main() -> int:
    parser = argparse.ArgumentParser(description="Звіт про брязкіт кнопки з запису логічного аналізатора")
    parser.add_argument("capture", type=Path, help="запис аналізатора: .vcd або .csv")
    parser.add_argument("--channel", help="назва або номер каналу (за замовчуванням перший, D0 = CH1)")
    parser.add_argument("--gap-ms", type=float, default=30,
                        help="тиша, що розділяє пачки брязкоту, мс (як BURST_GAP_MS у прошивці, 30)")
    parser.add_argument("--mcu-log", type=Path, help="лог Serial Monitor прошивки counter або debounce")
    parser.add_argument("--mcu-skip", type=int, default=0,
                        help="пропустити N перших натискань з логу МК, якщо запис аналізатора почато пізніше")
    parser.add_argument("--plots", type=Path,
                        help="папка для SVG-графіків кожного натискання (у звіт додається згорнута галерея)")
    parser.add_argument("--out", type=Path, help="записати markdown у файл")
    parser.add_argument("--update-readme", type=Path, help="вставити звіт між маркерами bounce-report у README")
    parser.add_argument("--section", help="маркери bounce-report:НАЗВА замість bounce-report (напр. 24msps-2)")
    args = parser.parse_args()

    try:
        loader = load_csv if args.capture.suffix.lower() == ".csv" else load_vcd
        signal = loader(args.capture, args.channel)
        if signal.initial == 0:
            first = f" до {signal.edges[0][0] * 1000:.0f} мс" if signal.edges else ""
            print(f"⚠️  запис починається з LOW{first}: прошивка ще не ввімкнула INPUT_PULLUP або кнопка натиснута",
                  file=sys.stderr)

        gap = args.gap_ms / 1000
        presses, skipped = group_presses(split_bursts(signal, gap))
        if not presses:
            raise ReportError(f"у записі каналу {signal.name} немає натискань ({len(signal.edges)} фронтів)")

        mcu_count, mcu_unit = None, "falling"
        if args.mcu_log:
            mcu, mcu_unit = load_mcu_log(args.mcu_log)
            mcu = mcu[args.mcu_skip:]
            mcu_count = len(mcu)
            for p, record in zip(presses, mcu):
                p.mcu = record

        plots_src = None
        if args.plots:
            # шлях до графіків відносно файлу, куди піде markdown
            base = (args.update_readme or args.out or Path("report.md")).parent
            plots_src = Path(os.path.relpath(args.plots, base)).as_posix()

        report = build_report(signal, args.capture.as_posix(), presses, skipped, gap,
                              args.mcu_log.as_posix() if args.mcu_log else None, mcu_count, mcu_unit, plots_src)

        if args.plots:
            write_plots(presses, args.plots)
            print(f"графіки: {args.plots}/press-01.svg … press-{len(presses):02d}.svg", file=sys.stderr)
        if args.out:
            args.out.write_text(report, encoding="utf-8")
        if args.update_readme:
            update_readme(args.update_readme, report, args.section)
            print(f"оновлено: {args.update_readme}", file=sys.stderr)
        print(report)
    except (OSError, ReportError) as e:
        print(f"✗ {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
