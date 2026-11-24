#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-build/intel-gpu-cpu-view-host}
mkdir -p "$out"
ulimit -c 0
for cc in gcc clang; do
    for mode in normal sanitize; do
        flags=()
        if [[ $mode == sanitize ]]; then
            flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        fi
        "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${flags[@]}" \
            test/intel_gpu_cpu_view_host.c drivers/intel_gpu_cpu_view.c \
            drivers/intel_gpu_kmt.c drivers/intel_gpu_buffer.c drivers/intel_gpu_memory.c \
            drivers/intel_bcs_context.c drivers/intel_rcs_sampled.c drivers/intel_rcs_vertex.c drivers/intel_gen9_texture.c drivers/intel_gen9_shader.c drivers/intel_rcs.c drivers/intel_rcs_draw.c drivers/intel_ppgtt.c drivers/intel_ggtt_space.c \
            drivers/intel_bcs.c drivers/intel_cs.c drivers/intel_bcs_queue.c \
            win32/d3dkmt_registry.c -o "$out/owner-$cc-$mode"
        timeout 60 "$out/owner-$cc-$mode" | tee "$out/$cc-$mode.log"
    done
done
