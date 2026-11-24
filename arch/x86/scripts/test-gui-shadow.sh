#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:-arch/x86/build/gui-shadow-host}
test ! -e "$out"
mkdir -p "$out"
cp --parents "$0" arch/x86/test/gui_shadow_host.c gui/gui_draw.c gui/gui_theme.c \
    gui/gui.h gui/gui_theme.h "$out/"
for compiler in gcc clang; do
    for mode in normal sanitize; do
        flags=(-std=c11 -O2 -g -Wall -Wextra -Werror)
        if [[ $mode == sanitize ]]; then
            flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        fi
        "$compiler" "${flags[@]}" arch/x86/test/gui_shadow_host.c \
            gui/gui_draw.c gui/gui_theme.c -o "$out/$compiler-$mode"
        "$out/$compiler-$mode" | tee "$out/$compiler-$mode.log"
    done
done
