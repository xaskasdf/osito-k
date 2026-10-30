#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:-build/gdi-font-lifetime}
mkdir -p "$out"
source=()
if [[ -n ${GDI_SURFACE_TEST_SOURCE:-} ]]; then
    source+=("-DGDI_SURFACE_TEST_SOURCE=\"$(realpath "$GDI_SURFACE_TEST_SOURCE")\"")
fi
for compiler in gcc clang; do
    for mode in normal sanitize; do
        flags=(-std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-address
               -ffunction-sections -fdata-sections -Iarch/x86/include
               -Iarch/x86/win32 -Iarch/x86/kernel -Iinclude/common -pthread)
        if [[ $mode == sanitize ]]; then
            flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
        fi
        "$compiler" "${flags[@]}" "${source[@]}" arch/x86/test/gdi_font_lifetime_host.c \
            -Wl,--gc-sections -o "$out/$compiler-$mode"
        timeout 60 "$out/$compiler-$mode" > "$out/$compiler-$mode.log" 2>&1
        cat "$out/$compiler-$mode.log"
    done
done
