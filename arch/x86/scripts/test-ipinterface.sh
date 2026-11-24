#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required};mkdir -p "$out"
for cc in gcc clang; do
    for mode in normal sanitize; do
        flags=()
        if [[ $mode == sanitize ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer);fi
        "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror -iquote arch/x86/include \
            -include arch/x86/test/variant_host_types.h "${flags[@]}" \
            -ffunction-sections -fdata-sections -Wl,--gc-sections -pthread \
            arch/x86/test/ipinterface_host.c arch/x86/win32/ipinterface.c arch/x86/kernel/crypto.c \
            -o "$out/ipinterface-$cc-$mode"
        "$out/ipinterface-$cc-$mode"
    done
done
