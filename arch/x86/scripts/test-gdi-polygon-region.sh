#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-build/gdi-polygon-region-host}
mkdir -p "$out"
flags=(-std=c11 -O2 -g -fno-builtin -DGDI_HOST_TEST=1 -Wall -Wextra -Wno-unused-parameter -Wno-address
       -ffunction-sections -fdata-sections -Iinclude -Iwin32 -Ikernel -I../../include/common)
for compiler in gcc clang; do
    for mode in normal sanitize; do
        extra=()
        if [[ $mode == sanitize ]]; then
            extra+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
        fi
        binary="$out/gdi-polygon-$compiler-$mode"
        "$compiler" "${flags[@]}" "${extra[@]}" test/gdi_polygon_region_host.c test/gdi_unreached_host.c \
            -Wl,--gc-sections,--wrap=malloc,--wrap=free -o "$binary"
        "$binary" | tee "$binary.log"
    done
done
