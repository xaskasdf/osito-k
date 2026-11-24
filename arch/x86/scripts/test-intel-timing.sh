#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:?output directory required}
mkdir -p "$out"
ulimit -c 0
export ASAN_OPTIONS=halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1
for cc in gcc clang; do
    for mode in normal sanitize; do
        flags=()
        if [[ $mode == sanitize ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
        "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror -fno-builtin "${flags[@]}" \
            -D__KERNEL_X86__=1 test/intel_timing_host.c drivers/intel_ggtt.c \
            -o "$out/timing-$cc-$mode"
        "$out/timing-$cc-$mode" | tee "$out/$cc-$mode.log"
    done
done
