#!/bin/bash
set -euo pipefail
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1
repo=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$repo"
out=${1:-arch/x86/build/keyboard-host}
test ! -e "$out"
mkdir -p "$out"
cp --parents "$0" arch/x86/kernel/keyboard.c arch/x86/kernel/input_events.c \
    arch/x86/include/input_events.h arch/x86/include/types.h arch/x86/drivers/ps2.h \
    arch/x86/test/keyboard_host.c arch/x86/kernel/terminal.c \
    arch/x86/test/terminal_host.c "$out/"
out=$(realpath "$out")
cd "$out"
for compiler in gcc clang; do
    for mode in normal sanitize; do
        flags=()
        if [[ $mode == sanitize ]]; then
            flags=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        fi
        "$compiler" -std=c11 -O2 -g -Wall -Wextra -Werror \
            "${flags[@]}" arch/x86/test/keyboard_host.c -o "$out/$compiler-$mode"
        "$out/$compiler-$mode" | tee "$out/$compiler-$mode.log"
        "$compiler" -std=c11 -O2 -g -Wall -Wextra -Werror \
            "${flags[@]}" arch/x86/test/terminal_host.c -o "$out/terminal-$compiler-$mode"
        "$out/terminal-$compiler-$mode" | tee "$out/terminal-$compiler-$mode.log"
    done
done
