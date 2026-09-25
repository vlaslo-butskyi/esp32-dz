#!/usr/bin/env bash
# Regenerates compile_commands.json so IntelliJ IDEA / CLion can resolve the code.
# Run it after every build_src_filter switch in platformio.ini.
set -euo pipefail
cd "$(dirname "$0")/.."

"$HOME/.platformio/penv/bin/pio" run -t compiledb

python3 - <<'PY'
import glob, json, os

bins = {}
for d in glob.glob(os.path.expanduser("~/.platformio/packages/toolchain-*/bin")):
    for f in os.listdir(d):
        bins.setdefault(f, os.path.join(d, f))

entries = json.load(open("compile_commands.json"))
fixed = 0
for e in entries:
    exe, sep, rest = e.get("command", "").partition(" ")
    if not os.path.isabs(exe) and exe in bins:
        e["command"] = bins[exe] + sep + rest
        fixed += 1

json.dump(entries, open("compile_commands.json", "w"), indent=1)
print(f"absolute compiler path set in {fixed}/{len(entries)} entries")
PY
