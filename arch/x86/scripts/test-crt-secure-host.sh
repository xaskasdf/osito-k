#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory}; mkdir -p "$out"
for cc in gcc clang; do
    for mode in normal sanitize; do
        flags=(); [[ $mode == sanitize ]] && flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror -fno-builtin "${flags[@]}" \
            arch/x86/test/crt_secure_host.c arch/x86/win32/crt_secure.c -o "$out/$cc-$mode"
        ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
            "$out/$cc-$mode" | tee "$out/$cc-$mode.log"
    done
done
