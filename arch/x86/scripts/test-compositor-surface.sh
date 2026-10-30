#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:-build/compositor-surface-host}
mkdir -p "$out"
for compiler in gcc clang; do
    for mode in normal sanitize; do
        flags=(-std=c11 -O2 -g -Wall -Wextra -pthread -ffunction-sections -fdata-sections
               -iquote arch/x86/kernel -Igui)
        if [[ $mode == sanitize ]]; then
            flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
        fi
        "$compiler" "${flags[@]}" arch/x86/test/compositor_surface_host.c \
            -Wl,--gc-sections -o "$out/$compiler-$mode"
        timeout 60 "$out/$compiler-$mode" | tee "$out/$compiler-$mode.log"
    done
done
