#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:?output directory required}
mkdir -p "$out"
ulimit -c 0
for cc in gcc clang; do
    for mode in normal sanitize; do
        flags=()
        if [[ $mode == sanitize ]]; then flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer); fi
        "$cc" -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-address \
            -ffunction-sections -fdata-sections -Iinclude -Iwin32 -Ikernel -I../../include/common \
            "${flags[@]}" test/gdi_adapter_names_host.c -Wl,--gc-sections -o "$out/gdi-$cc-$mode"
        "$out/gdi-$cc-$mode" | tee "$out/$cc-$mode.log"
    done
done
