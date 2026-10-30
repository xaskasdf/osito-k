#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-build/d3dkmt-registry-host}
mkdir -p "$out"
ulimit -c 0
for cc in gcc clang; do
    for mode in normal sanitize; do
        flags=()
        if [[ $mode == sanitize ]]; then
            flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        fi
        "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -fno-builtin -ffunction-sections -Wl,--gc-sections \
            "${flags[@]}" test/d3dkmt_device_host.c win32/d3dkmt_registry.c \
            win32/d3dkmt.c drivers/intel_bcs_queue.c drivers/intel_bcs.c drivers/intel_cs.c \
            drivers/intel_gpu_memory.c drivers/intel_gpu_buffer.c \
            -o "$out/registry-$cc-$mode"
        "$out/registry-$cc-$mode" | tee "$out/$cc-$mode.log"
        "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -fno-builtin -ffunction-sections -Wl,--gc-sections \
            "${flags[@]}" test/d3dkmt_allocation_host.c win32/d3dkmt_registry.c \
            win32/d3dkmt.c drivers/intel_bcs_queue.c drivers/intel_bcs.c drivers/intel_cs.c \
            drivers/intel_gpu_memory.c drivers/intel_gpu_buffer.c \
            -o "$out/allocation-$cc-$mode"
        timeout 60 "$out/allocation-$cc-$mode" | tee "$out/allocation-$cc-$mode.log"
        "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -fno-builtin -ffunction-sections -Wl,--gc-sections \
            "${flags[@]}" test/d3dkmt_context_host.c win32/d3dkmt_registry.c \
            win32/d3dkmt.c drivers/intel_bcs_queue.c drivers/intel_bcs.c drivers/intel_cs.c \
            drivers/intel_gpu_memory.c drivers/intel_gpu_buffer.c \
            -o "$out/context-$cc-$mode"
        timeout 60 "$out/context-$cc-$mode" | tee "$out/context-$cc-$mode.log"
    done
done
