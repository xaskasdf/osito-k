#!/usr/bin/env bash
set -euo pipefail
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1
repo=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$repo"
out=${1:-arch/x86/build/hwbp-trace-host}
test ! -e "$out"
mkdir -p "$out"
cp --parents "$0" arch/x86/test/hwbp_trace_host.c \
    arch/x86/kernel/{hwbp.c,hwbp_trace.h} \
    arch/x86/include/{hwbp.h,types.h,stdint.h,interrupt.h,paging.h} "$out/"
out=$(realpath "$out")
cd "$out"
for compiler in gcc clang; do
    for mode in normal sanitize; do
        flags=()
        if [[ $mode == sanitize ]]; then
            flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        fi
        "$compiler" -std=c11 -O2 -g -Wall -Wextra -Werror -Wno-unused-function \
            -ffunction-sections -fdata-sections -Wl,--gc-sections \
            "${flags[@]}" arch/x86/test/hwbp_trace_host.c -o "$out/$compiler-$mode"
        "$out/$compiler-$mode" | tee "$out/$compiler-$mode.log"
    done
done
