#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:-build/gui-aero-host}
test ! -e "$out"
mkdir -p "$out"
sources=(arch/x86/test/gui_aero_host.c arch/x86/test/gui_ui_host_stubs.c gui/*.c)
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
"$out/clang-normal" "$out/dock-1280.ppm" 1280 800 100 0 0
"$out/clang-normal" "$out/taskbar-1280.ppm" 1280 800 100 1 0
"$out/clang-normal" "$out/compact-800.ppm" 800 600 100 0 0
"$out/clang-normal" "$out/fractional-1920.ppm" 1920 1080 150 0 1
"$out/clang-normal" "$out/hidpi-3840.ppm" 3840 2160 200 1 0
