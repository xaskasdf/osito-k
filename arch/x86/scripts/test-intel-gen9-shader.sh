#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:?output directory required}
mkdir -p "$out"
ulimit -c 0
export ASAN_OPTIONS=halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
for cc in gcc clang; do
    for mode in normal sanitize; do
        flags=()
        [[ $mode == sanitize ]] && flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror "${flags[@]}" \
            test/intel_gen9_shader_host.c drivers/intel_gen9_shader.c drivers/intel_rcs_draw.c \
            -o "$out/shader-$cc-$mode"
        timeout 60 "$out/shader-$cc-$mode" build/native-rcs-program/compiled-fixtures.bin \
            build/native-rcs-primitive/compiled-fixtures.bin | tee "$out/$cc-$mode.log"
    done
done
