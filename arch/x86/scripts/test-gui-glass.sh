#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:-build/gui-glass-host}
test ! -e "$out"
mkdir -p "$out"
sources=(arch/x86/test/gui_glass_host.c arch/x86/test/gui_ui_host_stubs.c gui/*.c)
for compiler in gcc clang; do
    for mode in normal sanitize; do
        flags=(-std=c11 -O2 -g -Wall -Wextra -Werror)
        if [[ $mode == sanitize ]]; then
            flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        fi
        "$compiler" "${flags[@]}" "${sources[@]}" -o "$out/$compiler-$mode"
        "$out/$compiler-$mode" | tee "$out/$compiler-$mode.log"
    done
done
"$out/clang-normal" "$out/material.ppm"
"$out/clang-normal" --benchmark | tee "$out/benchmark.log"
