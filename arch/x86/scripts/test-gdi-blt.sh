#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-build/gdi-blt-host}
mkdir -p "$out"
flags=(-std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-address
       -ffunction-sections -fdata-sections -Iinclude -Iwin32 -Ikernel
       -I../../include/common)
for compiler in gcc clang; do
    for mode in normal sanitize; do
        extra=()
        if [[ $mode == sanitize ]]; then
            extra+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
        fi
        binary="$out/gdi-blt-$compiler-$mode"
        "$compiler" "${flags[@]}" "${extra[@]}" test/gdi_blt_host.c -Wl,--gc-sections -o "$binary"
        "$binary" | tee "$binary.log"
    done
    if [[ ${GDI_BLT_BENCH:-0} == 1 ]]; then
        "$out/gdi-blt-$compiler-normal" --bench | tee "$out/bench-$compiler-current.log"
        if [[ -n ${GDI_BLT_BASELINE_SOURCE:-} ]]; then
            baseline=$(realpath "$GDI_BLT_BASELINE_SOURCE")
            "$compiler" "${flags[@]}" "-DGDI_SCREEN_TEST_SOURCE=\"$baseline\"" \
                test/gdi_blt_host.c -Wl,--gc-sections -o "$out/gdi-blt-$compiler-baseline"
            "$out/gdi-blt-$compiler-baseline" --bench | tee "$out/bench-$compiler-baseline.log"
        fi
    fi
done
