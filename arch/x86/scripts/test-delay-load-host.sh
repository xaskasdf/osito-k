#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory};mkdir -p "$out"
for cc in gcc clang; do
    for mode in normal sanitize; do
        flags=()
        [[ $mode == sanitize ]] && flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        "$cc" -std=c11 -O1 -g -Wall -Wextra -Werror "${flags[@]}" \
            arch/x86/test/delay_load_host.c -o "$out/delay-$cc-$mode"
        ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$out/delay-$cc-$mode"
    done
done
