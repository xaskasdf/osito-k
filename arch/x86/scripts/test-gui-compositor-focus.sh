#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:-build/gui-compositor-focus}
test ! -e "$out"
mkdir -p "$out"
for compiler in gcc clang; do
    for mode in normal sanitize; do
        flags=(-std=gnu11 -O2 -g -ffunction-sections -fdata-sections)
        if [[ $mode == sanitize ]]; then
            flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        fi
        "$compiler" "${flags[@]}" -Igui -Iarch/x86/include \
            -c arch/x86/test/gui_compositor_focus_host.c -o "$out/$compiler-$mode.o"
        "$compiler" "${flags[@]}" -Igui \
            "$out/$compiler-$mode.o" arch/x86/test/gui_ui_host_stubs.c \
            gui/*.c -Wl,--gc-sections -o "$out/$compiler-$mode"
        "$out/$compiler-$mode" | tee "$out/$compiler-$mode.log"
    done
done
