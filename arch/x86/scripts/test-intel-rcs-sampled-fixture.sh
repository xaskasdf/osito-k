#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required}
mkdir -p "$out"
ulimit -c 0
export ASAN_OPTIONS=halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
for cc in gcc clang; do
    for mode in normal sanitize; do
        flags=()
        [[ $mode == sanitize ]] && flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        "$cc" -std=gnu11 -O2 -g -Wall -Wextra -Werror "${flags[@]}" \
            arch/x86/test/intel_rcs_sampled_fixture_host.c arch/x86/drivers/intel_rcs_sampled_test.c \
            arch/x86/drivers/intel_rcs_sampled.c arch/x86/drivers/intel_rcs_draw.c \
            arch/x86/drivers/intel_gen9_texture.c arch/x86/drivers/intel_gen9_shader.c \
            -o "$out/fixture-$cc-$mode"
        timeout 60 "$out/fixture-$cc-$mode" arch/x86/build/native-sampler/host-qualified "$out" | tee "$out/$cc-$mode.log"
    done
done
