#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:?output directory required}
mkdir -p "$out"
ulimit -c 0
for cc in gcc clang; do
    for mode in normal sanitize; do
        flags=()
        if [[ $mode == sanitize ]]; then
            flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
        fi
        # The production display translation unit also has unused blit helpers.
        "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror -Wno-unused-function -fno-builtin \
            -ffunction-sections -fdata-sections "${flags[@]}" \
            test/display_identity_host.c -Wl,--gc-sections -o "$out/identity-$cc-$mode"
        "$out/identity-$cc-$mode" | tee "$out/$cc-$mode.log"
    done
done
