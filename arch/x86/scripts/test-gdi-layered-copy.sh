#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-build/gdi-layered-copy-host}
mkdir -p "$out"
flags=(-std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-address
       -ffunction-sections -fdata-sections -Iinclude -Iwin32 -Ikernel -I../../include/common)
for compiler in gcc clang; do
    for mode in normal sanitize; do
        extra=()
        if [[ $mode == sanitize ]]; then
            extra+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
        fi
        binary="$out/gdi-layered-$compiler-$mode"
        "$compiler" "${flags[@]}" "${extra[@]}" test/gdi_layered_copy_host.c -Wl,--gc-sections -o "$binary"
        "$binary" | tee "$binary.log"
    done
    if [[ ${GDI_LAYERED_BENCH:-0} == 1 ]]; then
        "$out/gdi-layered-$compiler-normal" --bench | tee "$out/bench-$compiler-current.log"
        if [[ -n ${GDI_LAYERED_BASELINE_SOURCE:-} ]]; then
            baseline=$(realpath "$GDI_LAYERED_BASELINE_SOURCE")
            "$compiler" "${flags[@]}" "-DGDI_SCREEN_TEST_SOURCE=\"$baseline\"" \
                test/gdi_layered_copy_host.c -Wl,--gc-sections -o "$out/gdi-layered-$compiler-baseline"
            "$out/gdi-layered-$compiler-baseline" | tee "$out/baseline-$compiler.log"
            "$out/gdi-layered-$compiler-baseline" --bench | tee "$out/bench-$compiler-baseline.log"
        fi
    fi
done
