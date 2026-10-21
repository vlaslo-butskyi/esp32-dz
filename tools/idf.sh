#!/bin/sh
# Обгортка над idf.py для прошивок на нативному ESP-IDF (ДЗ 2.6 і далі), не PlatformIO.
#
#   ./tools/idf.sh "dz2.6 (ESP-IDF DEBOUNCE)/raw_interrupt" build
#   ./tools/idf.sh "dz2.6 (ESP-IDF DEBOUNCE)/raw_interrupt" flash monitor
#   ./tools/idf.sh "dz2.6 (ESP-IDF DEBOUNCE)/raw_interrupt" menuconfig
#
# ESP-IDF поставлений через eim (ESP-IDF Installation Manager):
#   brew tap espressif/eim && brew install --cask eim-gui
#   eim install -n true -i v5.5.5 -t esp32s3
# Версія тут прибита свідомо — ДЗ зібране й зміряне саме на ній. Інша версія:
#   IDF_ACTIVATE=~/.espressif/tools/activate_idf_v6.1.sh ./tools/idf.sh ...
#   (перелік установлених: eim list)
#
# Обгортка робить три речі:
#
# 1. Активує середовище скриптом від eim. Він сам виставляє IDF_PATH,
#    IDF_TOOLS_PATH і IDF_PYTHON_ENV_PATH і не викликає export.sh, тож
#    PlatformIO-venv із ~/.zshrc йому не мішає.
#
# 2. Викликає idf.py без exec: активаційний скрипт оголошує його shell-функцією,
#    а не кладе на PATH, і exec на функції не працює.
#
# 3. Виносить build-каталог у ~/.cache/idf-<номер ДЗ>/<варіант>. У назві папки ДЗ є
#    дужки, а CMake і ninja підставляють шляхи в /bin/sh, екрануючи пробіли й
#    не екрануючи "(" — збірка всередині папки ДЗ падає з
#    "syntax error near unexpected token `('". Заодно в репо не лишається build/.
set -eu

if [ $# -lt 2 ]; then
  echo "usage: $0 <папка прошивки> <аргументи idf.py...>" >&2
  exit 2
fi

ACTIVATE="${IDF_ACTIVATE:-$HOME/.espressif/tools/activate_idf_v5.5.5.sh}"
PORT="${ESPPORT:-/dev/cu.usbmodem5C371991551}"

if [ ! -f "$ACTIVATE" ]; then
  echo "немає активаційного скрипта ESP-IDF: $ACTIVATE" >&2
  echo "постав версію через eim або вкажи свою в IDF_ACTIVATE" >&2
  exit 1
fi

project="$1"
shift
variant=$(basename "$project")
# "dz2.7 (TRAFFIC LIGHT)/traffic_light" -> ~/.cache/idf-dz2.7/traffic_light
dz=$(basename "$(dirname "$project")")
dz=${dz%% *}
case "$dz" in dz*) ;; *) dz=dz ;; esac
build_dir="${IDF_BUILD_DIR:-$HOME/.cache/idf-$dz/$variant}"

env -u VIRTUAL_ENV -u PYTHONPATH -u PYTHONHOME -u IDF_PATH \
  PATH="/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin" \
  HOME="$HOME" ESPPORT="$PORT" ACTIVATE="$ACTIVATE" \
  zsh -f -c '
    . "$ACTIVATE" > /dev/null 2>&1 || exit 1
    cd "$1" || exit 1
    build_dir="$2"
    shift 2
    idf.py -B "$build_dir" "$@"
  ' idf.sh "$project" "$build_dir" "$@"
