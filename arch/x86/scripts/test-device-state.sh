#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:?output directory}; mkdir -p "$out"; ulimit -c 0
for cc in gcc clang; do
    for mode in normal sanitize; do
        flags=(); [[ $mode == sanitize ]] && flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        common=(-std=c11 -O2 -g -Wall -Wextra -Werror -pthread -fno-builtin -ffunction-sections -Wl,--gc-sections)
        "$cc" "${common[@]}" "${flags[@]}" test/d3dkmt_device_state_host.c win32/d3dkmt_registry.c win32/d3dkmt.c \
            drivers/intel_bcs_queue.c drivers/intel_bcs.c drivers/intel_cs.c drivers/intel_gpu_memory.c \
            drivers/intel_gpu_buffer.c -o "$out/public-$cc-$mode"
        timeout 60 "$out/public-$cc-$mode" | tee "$out/public-$cc-$mode.log"
    done
done
