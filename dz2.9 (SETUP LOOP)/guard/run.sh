#!/bin/sh
# Чотири випадки того самого циклу app.h <-> board.h.
# Компілятор хоста, нічого з STM32 не потрібно:  ./run.sh
set -u
cd "$(dirname "$0")"

check() {
    name=$1
    shift
    printf '\n== %s ==\n   cc %sdemo.c\n' "$name" "$(test $# -gt 0 && printf '%s ' "$@")"
    if out=$(cc -fsyntax-only -Wall "$@" demo.c 2>&1); then
        printf '   OK, зібралось\n'
    else
        printf '%s\n' "$out" | grep -E 'error:' | head -2 | sed 's/^/   /'
    fi
}

check "сторожі вимкнені"           -DNO_GUARD
check "сторожі є, цикл є"
check "те саме, інший порядок"     -DBOARD_FIRST
check "цикл розірвано оголошенням" -DFIXED
printf '\n'
