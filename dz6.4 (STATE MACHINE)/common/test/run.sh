#!/bin/sh
# Тести спільного коду ДЗ 6.4 на ПК. Потрібен тільки компілятор C (clang або gcc).
#   ./run.sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
${CC:-cc} -std=c11 -Wall -Wextra -Wpedantic -Werror -I"$here/.." \
  "$here/test_fsm.c" "$here/../button_fsm.c" "$here/../safe_fsm.c" -o "$out/test_fsm"
"$out/test_fsm"
