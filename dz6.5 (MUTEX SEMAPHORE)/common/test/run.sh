#!/bin/sh
# Тести спільного коду ДЗ 6.5 на ПК. Потрібен тільки компілятор C (clang або gcc).
#   ./run.sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
${CC:-cc} -std=c11 -Wall -Wextra -Wpedantic -Werror -I"$here/.." \
  "$here/test_conveyor.c" "$here/../press_filter.c" "$here/../rate.c" "$here/../report.c" -o "$out/test_conveyor"
"$out/test_conveyor"
