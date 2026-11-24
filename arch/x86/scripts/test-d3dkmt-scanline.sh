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
        [[ $mode == sanitize ]] && flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -fno-builtin -ffunction-sections -Wl,--gc-sections \
            "${flags[@]}" test/d3dkmt_scanline_host.c win32/d3dkmt_registry.c win32/d3dkmt.c \
            drivers/intel_bcs_queue.c drivers/intel_bcs.c drivers/intel_cs.c \
            drivers/intel_gpu_memory.c drivers/intel_gpu_buffer.c -o "$out/scanline-$cc-$mode"
        timeout 60 "$out/scanline-$cc-$mode" | tee "$out/$cc-$mode.log"
        "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror "${flags[@]}" \
            test/intel_gfx_display_host.c drivers/intel_gfx_display.c -o "$out/provider-$cc-$mode"
        "$out/provider-$cc-$mode" | tee "$out/provider-$cc-$mode.log"
    done
done
