#!/usr/bin/env python3
"""Запис SEGGER SystemView з ESP32-S3 через вбудований USB-JTAG (ДЗ 6.2 і далі).

    python3 tools/sysview.py "dz6.2 (RTOS SCHEDULER)/four_tasks_sysview"
    python3 tools/sysview.py "dz6.2 (RTOS SCHEDULER)/four_tasks_sysview" 5
    python3 tools/sysview.py "dz6.2 (RTOS SCHEDULER)/four_tasks_sysview" split
    SYSVIEW_DEBUG=1 python3 tools/sysview.py ...   # плюс повний лог OpenOCD

Скрипт запускає OpenOCD з board/esp32s3-builtin.cfg, через Tcl-порт 6666
скидає плату (reset run), вмикає `esp sysview_mcore` на задану кількість
секунд і кладе trace.SVDat у папку прошивки. Обидва ядра йдуть в один файл
у багатоядерному форматі SystemView 3.60+.

Поруч скрипт кладе trace_core1.SVDat: тільки ядро 1 в одноядерному форматі.
ESP-IDF шле Task Info кожної задачі в потоки обох ядер з однаковим часом.
SystemView 4.10b віддає задачу тому ядру, де першим побачив її ім'я, тобто
ядру 0. Тоді запуски задач на ядрі 1 SystemView показує як безіменні
"Task 0x...", а рядки sensor, event, worker порожні. В одноядерному файлі
цього конфлікту немає. Аргумент `split` робить тільки trace_core1.SVDat
з наявного trace.SVDat, без запису з плати.

Плата підключається кабелем у порт USB, не UART: JTAG є тільки там.

OpenOCD береться той, що eim поставив разом з ESP-IDF 5.5.5. Пакети SystemView
з ESP-IDF 5.5 розуміє тільки OpenOCD з цієї ж поставки. Інший шлях:
    OPENOCD_ESP32=~/.espressif/tools/openocd-esp32/<версія>/openocd-esp32 ...

Сокет керується вручну, бо `sleep` у `-c` блокує цикл OpenOCD, і дані
трасування в цей час не забираються з плати.
"""
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

OPENOCD = os.path.expanduser(os.environ.get(
    "OPENOCD_ESP32",
    "~/.espressif/tools/openocd-esp32/v0.12.0-esp32-20260424/openocd-esp32"))
TCL_PORT = 6666
EOT = b"\x1a"


def tcl(sock, cmd):
    sock.sendall(cmd.encode() + EOT)
    data = b""
    while not data.endswith(EOT):
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("OpenOCD закрив з'єднання на команді: %s" % cmd)
        data += chunk
    return data[:-1].decode(errors="replace").strip()


def app_main_addr(variant_dir):
    """Адреса app_main з ELF, який зібрав tools/idf.sh для цієї папки."""
    dz = os.path.basename(os.path.dirname(variant_dir)).split(" ")[0]
    name = os.path.basename(variant_dir)
    elf = os.path.expanduser("~/.cache/idf-%s/%s/%s.elf" % (dz, name, name))
    nm = shutil.which("xtensa-esp32s3-elf-nm") or os.path.expanduser(
        "~/.platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-nm")
    for line in subprocess.check_output([nm, elf], text=True).splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == "app_main":
            return "0x" + parts[0]
    sys.exit("немає app_main у %s" % elf)


def split_core1(trace):
    """З багатоядерного trace.SVDat робить trace_core1.SVDat (тільки ядро 1)."""
    data = open(trace, "rb").read()
    head = 0
    while data[head:head + 1] == b";":
        head = data.index(b"\n", head) + 1
    m = re.search(rb"; Offset Core1 (\d+)", data[:head])
    if not m:
        sys.exit("%s: у заголовку немає Offset Core1" % trace)
    # Зсув рахується від кінця заголовка. Потік ядра 1 починається з власних
    # Init і System Description, тому заголовок одноядерного формату достатній.
    header = b"; Version     SEGGER SystemViewer V2.42\n; Author      Espressif Inc\n;\n;\n"
    out = os.path.join(os.path.dirname(trace), "trace_core1.SVDat")
    open(out, "wb").write(header + data[head + int(m.group(1)):])
    print("%s: %d байт" % (out, os.path.getsize(out)))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    out_dir = os.path.abspath(sys.argv[1])
    out = os.path.join(out_dir, "trace.SVDat")
    if len(sys.argv) > 2 and sys.argv[2] == "split":
        split_core1(out)
        return
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 10

    global APP_MAIN
    APP_MAIN = app_main_addr(out_dir)

    exe = os.path.join(OPENOCD, "bin", "openocd")
    if not os.path.isfile(exe):
        sys.exit("немає OpenOCD: %s" % exe)

    # OpenOCD бере шлях як URL file://, де пробіл і дужка в назві папки ДЗ
    # ламають розбір. Тому запис іде в тимчасову папку, потім файл переноситься.
    tmp = tempfile.mkdtemp(prefix="sysview-")
    tmp_out = os.path.join(tmp, "trace.SVDat")
    log_path = os.path.join(tmp, "openocd.log")

    with open(log_path, "w") as log:
        ocd = subprocess.Popen(
            [exe, "-s", os.path.join(OPENOCD, "share", "openocd", "scripts"),
             "-f", "board/esp32s3-builtin.cfg", "-c", "tcl_port %d" % TCL_PORT],
            stdout=log, stderr=subprocess.STDOUT)
    try:
        sock = None
        for _ in range(50):
            if ocd.poll() is not None:
                break
            try:
                sock = socket.create_connection(("127.0.0.1", TCL_PORT), timeout=30)
                break
            except OSError:
                time.sleep(0.2)
        if sock is None:
            print(open(log_path).read())
            sys.exit("OpenOCD не запустився, лог вище")

        # Як у прикладі ESP-IDF sysview_tracing: зупинити ядро на app_main і
        # тільки тоді вмикати запис. Після `reset run` у буфері на платі вже
        # лежать події завантаження, і OpenOCD відкидає їх: "Invalid init seq".
        tcl(sock, "reset halt")
        tcl(sock, "bp %s 2 hw" % APP_MAIN)
        tcl(sock, "resume")
        tcl(sock, "wait_halt 5000")
        tcl(sock, "rbp %s" % APP_MAIN)
        print(tcl(sock, "esp sysview_mcore start file://%s" % tmp_out))
        tcl(sock, "resume")
        print("запис %g с..." % seconds)
        time.sleep(seconds)
        print(tcl(sock, "esp sysview_mcore stop"))
        tcl(sock, "shutdown")
        sock.close()
        ocd.wait(timeout=10)
    finally:
        if ocd.poll() is None:
            ocd.kill()

    if not os.path.isfile(tmp_out) or os.path.getsize(tmp_out) == 0:
        print(open(log_path).read())
        sys.exit("trace.SVDat порожній, лог OpenOCD вище")
    shutil.move(tmp_out, out)
    if os.environ.get("SYSVIEW_DEBUG"):
        print(open(log_path).read())
    shutil.rmtree(tmp, ignore_errors=True)
    print("%s: %d байт" % (out, os.path.getsize(out)))
    split_core1(out)


if __name__ == "__main__":
    main()
