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
        "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -fno-builtin -ffunction-sections -Wl,--gc-sections \
            -Wl,--wrap=malloc -Wl,--wrap=free "${flags[@]}" \
            test/d3dkmt_display_modes_host.c win32/d3dkmt_registry.c win32/d3dkmt.c \
            drivers/intel_bcs_queue.c drivers/intel_bcs.c drivers/intel_cs.c \
            drivers/intel_gpu_memory.c drivers/intel_gpu_buffer.c -o "$out/modes-$cc-$mode"
        timeout 60 "$out/modes-$cc-$mode" | tee "$out/$cc-$mode.log"
    done
done
