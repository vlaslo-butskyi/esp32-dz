#!/usr/bin/env python3
"""Малює timeline.svg з trace.SVDat: три моменти ДЗ 6.2 на ядрі 1.

    python3 "dz6.2 (RTOS SCHEDULER)/four_tasks_sysview/plot_trace.py"

Події розбирає парсер ESP-IDF 5.5.5 (tools/esp_app_trace/sysviewtrace_proc.py).
Він читає тільки окремі файли на кожне ядро, а OpenOCD пише один файл
багатоядерного формату. Тому скрипт ділить файл за рядком `Offset Core1` і
дописує кожній половині заголовок старого формату.

Відрізок роботи задачі - від svTaskStartExec (або svIdle) до наступного
входу в переривання на тому ж ядрі. Без залежностей: SVG пишеться руками.
"""
import os
import re
import subprocess
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
IDF = os.path.expanduser("~/.espressif/v5.5.5/esp-idf")
PY = os.path.expanduser("~/.espressif/tools/python/v5.5.5/venv/bin/python")
ELF = os.path.expanduser("~/.cache/idf-dz6.2/four_tasks_sysview/four_tasks_sysview.elf")
TOOLCHAIN = os.path.expanduser(
    "~/.platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-")

CORE = 1
ROWS = [  # зверху вниз за пріоритетом
    ("maintenance", "maintenance", "Realtime, 4"),
    ("worker", "worker", "High, 3"),
    ("sensor", "sensor", "Normal, 2"),
    ("event", "event", "Low, 1"),
    ("IDLE1", "idle", "0"),
]
INK = "#2563c9"
IDLE_INK = "#b8bec8"
TEXT = "#1f2328"
MUTED = "#59636e"
GRID = "#e3e6ea"


def dump_events(trace):
    data = open(trace, "rb").read()
    head = 0
    while data[head:head + 1] == b";":
        head = data.index(b"\n", head) + 1
    off1 = int(re.search(rb"Offset Core1 (\d+)", data).group(1))
    old_header = b"; Version     SEGGER SystemViewer V2.42\n; Author      Espressif Inc\n;\n;\n"
    tmp = tempfile.mkdtemp(prefix="svplot-")
    parts = []
    for i, chunk in enumerate((data[head:head + off1], data[head + off1:])):
        path = os.path.join(tmp, "core%d.svdat" % i)
        open(path, "wb").write(old_header + chunk)
        parts.append("file://" + path)
    tool = os.path.join(IDF, "tools", "esp_app_trace")
    out = subprocess.run(
        [PY, os.path.join(tool, "sysviewtrace_proc.py"), "-w", "0", "-d",
         "-b", ELF, "--toolchain", TOOLCHAIN] + parts,
        capture_output=True, text=True, env=dict(os.environ, PYTHONPATH=tool), check=True)
    return out.stdout


def task_spans(dump):
    """{ім'я задачі: [(початок_мс, кінець_мс), ...]} для ядра CORE."""
    names, spans = {}, {}
    running, since = None, None
    pat = re.compile(r"EVENT\[\d+\]: ([\d.]+) - core\[(\d)\]\.(\w+)\(\d+\), plen \d+: \[(.*)\]")
    for line in dump.splitlines():
        m = pat.match(line)
        if not m:
            continue
        t, core, ev, args = float(m[1]) * 1000, int(m[2]), m[3], m[4]
        if ev == "svTaskInfo":
            names[int(re.search(r"tid: (\d+)", args)[1])] = re.search(r"name: (\S+)", args)[1]
        if core != CORE:
            continue
        if ev in ("svIsrEnter", "svTraceStop") and running is not None:
            spans.setdefault(running, []).append((since, t))
            running = None
        elif ev == "svTaskStartExec":
            running = names.get(int(re.search(r"tid: (\d+)", args)[1]), "?")
            since = t
        elif ev == "svIdle":
            running, since = "IDLE1", t
    return spans


def panel(spans, y0, t0, t1, title, ticks, tick_fmt, notes=(), min_w=2.0, idle=False):
    left, width, row_h = 190, 760, 26
    rows = [r for r in ROWS if idle or r[0] != "IDLE1"]
    x = lambda t: left + (t - t0) / (t1 - t0) * width
    svg = ['<text x="16" y="%d" font-size="15" font-weight="600" fill="%s">%s</text>'
           % (y0, TEXT, title)]
    top = y0 + 30
    bottom = top + row_h * len(rows)
    for t in ticks:
        svg.append('<line x1="%.1f" y1="%d" x2="%.1f" y2="%d" stroke="%s"/>'
                   % (x(t), top, x(t), bottom, GRID))
        svg.append('<text x="%.1f" y="%d" font-size="11" fill="%s" text-anchor="middle">%s</text>'
                   % (x(t), bottom + 14, MUTED, tick_fmt(t)))
    for i, (key, label, prio) in enumerate(rows):
        y = top + i * row_h
        svg.append('<text x="16" y="%d" font-size="13" fill="%s">%s</text>' % (y + 17, TEXT, label))
        svg.append('<text x="110" y="%d" font-size="11" fill="%s">%s</text>' % (y + 17, MUTED, prio))
        svg.append('<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="%s"/>'
                   % (left, y + row_h, left + width, y + row_h, GRID))
        color = IDLE_INK if key == "IDLE1" else INK
        for s, e in spans.get(key, []):
            if e < t0 or s > t1:
                continue
            xs, xe = x(max(s, t0)), x(min(e, t1))
            svg.append('<rect x="%.1f" y="%d" width="%.1f" height="%d" rx="2" fill="%s"/>'
                       % (xs, y + 5, max(xe - xs, min_w), row_h - 10, color))
    for t, row, text, anchor in notes:
        i = [r[0] for r in rows].index(row)
        svg.append('<text x="%.1f" y="%d" font-size="11" fill="%s" text-anchor="%s">%s</text>'
                   % (x(t), top + i * row_h - 3, TEXT, anchor, text))
    return svg, bottom + 52


def main():
    spans = task_spans(dump_events(os.path.join(HERE, "trace.SVDat")))
    out = []
    y = 30
    part, y = panel(spans, y, 0, 3600,
                    "1. sensor кожні 500 мс, worker на 3 с (0–3,6 с)",
                    range(0, 3601, 500), lambda t: ("%g с" % (t / 1000)).replace(".", ","))
    out += part
    t0 = 2997.95
    part, y = panel(spans, y, t0, t0 + 0.75,
                    "2. worker витісняє event (2998,0 мс, вікно 0,75 мс)",
                    [t0 + d / 1000 for d in range(50, 751, 100)],
                    lambda t: "%d мкс" % round((t - 2998.0) * 1000),
                    notes=[(2998.265, "event", "створює worker", "middle"),
                           (2998.329, "worker", "worker витісняє event", "start"),
                           (2998.571, "event", "event продовжує", "start")],
                    min_w=1.0, idle=True)
    out += part
    part, y = panel(spans, y, 9400, 12700,
                    "3. maintenance зупиняє sensor на 2 с (9,4–12,7 с)",
                    range(9500, 12701, 500), lambda t: ("%g с" % (t / 1000)).replace(".", ","),
                    notes=[(9998, "maintenance", "vTaskSuspend", "middle"),
                           (11998, "maintenance", "vTaskResume", "middle")])
    out += part
    svg = ['<svg xmlns="http://www.w3.org/2000/svg" width="980" height="%d" '
           'font-family="-apple-system, Segoe UI, Helvetica, Arial, sans-serif">' % y,
           '<rect width="100%" height="100%" fill="#ffffff"/>'] + out + ["</svg>"]
    path = os.path.join(HERE, "timeline.svg")
    open(path, "w").write("\n".join(svg))
    print(path)


if __name__ == "__main__":
    main()
