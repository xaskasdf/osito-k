#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required}
mkdir -p "$out"
for cc in gcc clang; do
    for mode in normal sanitize; do
        flags=()
        if [[ $mode == sanitize ]]; then
            flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
            # Do not retain unrelated OLE export tables in this focused host
            # harness. Heap, stack and undefined-behavior checks stay enabled.
            if [[ $cc == gcc ]]; then flags+=(--param asan-globals=0)
            else flags+=(-mllvm -asan-globals=0); fi
        fi
        "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
            -iquote arch/x86/include -include arch/x86/test/variant_host_types.h \
            "${flags[@]}" arch/x86/test/variant_host.c arch/x86/win32/variant.c \
            arch/x86/win32/ole32_shim.c arch/x86/win32/crt_float.c arch/x86/win32/wintime.c \
            -Wl,--gc-sections -o "$out/variant-$cc-$mode"
        "$out/variant-$cc-$mode"
    done
done
