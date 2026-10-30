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
            test/intel_gen9_sampler_host.c drivers/intel_gen9_shader.c drivers/intel_rcs_draw.c \
            -o "$out/sampler-$cc-$mode"
        timeout 60 "$out/sampler-$cc-$mode" build/native-sampler/host-qualified | tee "$out/$cc-$mode.log"
    done
done
