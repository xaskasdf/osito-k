#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:-build/compositor-frame-link}
mkdir -p "$out"
for compiler in gcc clang; do
    for mode in normal sanitize; do
        flags=(-std=gnu11 -O2 -g -ffunction-sections -fdata-sections
               -Iarch/x86/include -Igui)
        if [[ $mode == sanitize ]]; then
            flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
        fi
        "$compiler" "${flags[@]}" arch/x86/test/compositor_frame_link_host.c \
            arch/x86/test/compositor_frame_link_probe.c -Wl,--gc-sections \
            -o "$out/$compiler-$mode"
        "$out/$compiler-$mode" | tee "$out/$compiler-$mode.log"
    done
done
