#!/bin/sh
# Прогін серії натискань у симуляторі Wokwi для прошивок ДЗ 2.6.
#
#   sim/run.sh                 # усі шість варіантів
#   sim/run.sh raw_interrupt   # один
#
# Для кожного варіанту: збирає прошивку через PlatformIO (щоб firmware.bin ліг
# туди, куди дивиться wokwi.toml), проганяє сценарій sim/series20.yaml і
# складає serial.log та wokwi.vcd у папку варіанту, після чого друкує розбір.
#
# Токен береться з WOKWI_CLI_TOKEN або з ~/.wokwi/ci.tok (створюється на
# https://wokwi.com/dashboard/ci, починається з wok_, 44 символи).
set -eu

sim_dir=$(cd "$(dirname "$0")" && pwd)
dz_dir=$(dirname "$sim_dir")
repo_root=$(dirname "$dz_dir")
cli="${WOKWI_CLI:-$HOME/.wokwi/bin/wokwi-cli}"

# pio може бути не на PATH (у ~/.zshrc він додається, але скрипт запускається sh)
pio="pio"
command -v pio >/dev/null 2>&1 || pio="$HOME/.platformio/penv/bin/pio"

if [ -z "${WOKWI_CLI_TOKEN:-}" ] && [ -f "$HOME/.wokwi/ci.tok" ]; then
  WOKWI_CLI_TOKEN=$(tr -d '\r\n' < "$HOME/.wokwi/ci.tok")
  export WOKWI_CLI_TOKEN
fi

if [ -z "${WOKWI_CLI_TOKEN:-}" ]; then
  echo "немає WOKWI_CLI_TOKEN: створи токен на https://wokwi.com/dashboard/ci" >&2
  echo "і поклади його в ~/.wokwi/ci.tok" >&2
  exit 1
fi

if [ ! -x "$cli" ]; then
  echo "немає wokwi-cli: $cli" >&2
  exit 1
fi

variants=${*:-"raw_interrupt time_debounce state_debounce polling_debounce timer_debounce glitch_filter"}

for variant in $variants; do
  echo
  echo "===== $variant ====="
  ( cd "$repo_root" && "$pio" run -e "idf_$variant" >/dev/null )

  # Серія 1: брязкіт кнопки увімкнений (модель Wokwi) — перевірка фільтрів.
  "$cli" "$dz_dir/$variant" \
    --scenario "$sim_dir/series20.yaml" \
    --serial-log-file "$dz_dir/$variant/serial.log" \
    --vcd-file "$dz_dir/$variant/wokwi.vcd" \
    --timeout 60000 \
    --quiet

  # Серія 2: брязкіт вимкнений ("bounce": "0") — чиста затримка реакції.
  "$cli" "$dz_dir/$variant" \
    --diagram-file "$sim_dir/diagram-nobounce.json" \
    --scenario "$sim_dir/series20.yaml" \
    --serial-log-file "$dz_dir/$variant/serial-clean.log" \
    --vcd-file "$dz_dir/$variant/wokwi-clean.vcd" \
    --timeout 60000 \
    --quiet

  {
    echo "## $variant"
    echo
    echo "### З брязкотом"
    echo
    python3 "$sim_dir/analyze_vcd.py" "$dz_dir/$variant/wokwi.vcd" --markdown
    echo
    echo "### Без брязкоту"
    echo
    python3 "$sim_dir/analyze_vcd.py" "$dz_dir/$variant/wokwi-clean.vcd" --markdown
  } | tee "$sim_dir/report-$variant.md" | grep -E "натискань|фронтів|перемикань|затримка|реакцій|###"
done
