#!/bin/sh
# Тести спільного коду ДЗ 6.3 на ПК. Потрібен тільки компілятор C (clang або gcc).
#   ./run.sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
${CC:-cc} -std=c11 -Wall -Wextra -Wpedantic -Werror -I"$here/.." \
  "$here/test_queue.c" "$here/../consumer.c" "$here/../press_filter.c" -o "$out/test_queue"
"$out/test_queue"
