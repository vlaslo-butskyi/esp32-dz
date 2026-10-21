#!/bin/sh
# Еталон для порівняння: Arduino-прошивка raw_interrupt із ДЗ 2.4, прогнана
# тим самим сценарієм і в тому самому симуляторі, що й прошивки ДЗ 2.6.
# Без цього порівнювати затримку IDF із числами 2.4 не можна — там заміри
# з плати, тут із симулятора.
set -eu

sim_dir=$(cd "$(dirname "$0")" && pwd)
repo_root=$(dirname "$(dirname "$sim_dir")")
arduino="$repo_root/dz2.4 (BUTTON DEBOUNCE)/raw_interrupt"
cli="${WOKWI_CLI:-$HOME/.wokwi/bin/wokwi-cli}"
out="$sim_dir/arduino-baseline"

pio="pio"
command -v pio >/dev/null 2>&1 || pio="$HOME/.platformio/penv/bin/pio"

if [ -z "${WOKWI_CLI_TOKEN:-}" ] && [ -f "$HOME/.wokwi/ci.tok" ]; then
  WOKWI_CLI_TOKEN=$(tr -d '\r\n' < "$HOME/.wokwi/ci.tok")
  export WOKWI_CLI_TOKEN
fi

mkdir -p "$out"

# Arduino-env збирає ту прошивку, яку вибрано в platformio.ini, тому підміняємо
# фільтр на час збірки: змінна додається до наявного, тож поточний файл
# доводиться виключити явно.
active=$(grep -E '^build_src_filter' "$repo_root/platformio.ini" | sed 's/^build_src_filter = //')
( cd "$repo_root" && PLATFORMIO_BUILD_SRC_FILTER="-$(echo "$active" | sed 's/^+//') +<../dz2.4 (BUTTON DEBOUNCE)/raw_interrupt/main.cpp>" \
    "$pio" run -e esp32-s3-devkitc-1 >/dev/null )

"$cli" "$arduino" --scenario "$sim_dir/series20-arduino.yaml" \
  --serial-log-file "$out/serial.log" --vcd-file "$out/wokwi.vcd" \
  --timeout 60000 --quiet

"$cli" "$arduino" --diagram-file "$sim_dir/diagram-nobounce.json" \
  --scenario "$sim_dir/series20-arduino.yaml" \
  --serial-log-file "$out/serial-clean.log" --vcd-file "$out/wokwi-clean.vcd" \
  --timeout 60000 --quiet

python3 "$sim_dir/analyze_vcd.py" "$out/wokwi.vcd" --csv "$out/digital.csv" --markdown
python3 "$sim_dir/analyze_vcd.py" "$out/wokwi-clean.vcd" --csv "$out/digital-clean.csv" --markdown
