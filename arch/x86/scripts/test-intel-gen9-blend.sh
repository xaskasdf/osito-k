#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory}
mkdir -p "$out"
ulimit -c 0
export ASAN_OPTIONS=halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
for cc in gcc clang; do
    for mode in normal sanitize; do
        flags=()
        [[ $mode == sanitize ]] && flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror "${flags[@]}" \
            arch/x86/test/intel_gen9_blend_host.c arch/x86/drivers/intel_rcs_draw.c -o "$out/blend-$cc-$mode"
        "$out/blend-$cc-$mode" | tee "$out/$cc-$mode.log"
    done
done
