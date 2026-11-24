#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-build/boot-diag-host}
mkdir -p "$out"
export UBSAN_OPTIONS=halt_on_error=1
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
for compiler in gcc clang; do
    for mode in normal sanitize; do
        flags=()
        if [[ $mode == sanitize ]]; then
            flags=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        fi
        "$compiler" -std=c11 -O2 -g -Wall -Wextra -Werror \
            -ffunction-sections -fdata-sections -Wl,--gc-sections \
            "${flags[@]}" test/boot_checkpoint_host.c -o "$out/$compiler-$mode"
        "$out/$compiler-$mode" | tee "$out/$compiler-$mode.log"
    done
done
