#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:?output directory}
program=${2:?compiled uniform-c0.program fixture}
mkdir -p "$out"
ulimit -c 0
for cc in gcc clang; do
    for mode in normal sanitize; do
        flags=()
        if [[ $mode == sanitize ]]; then
            flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        fi
        "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${flags[@]}" \
            test/intel_gpu_constants_host.c drivers/intel_gpu_capture.c drivers/intel_gen9_shader.c drivers/intel_gpu_jobs.c \
            drivers/intel_gpu_bcs_owner.c drivers/intel_gpu_executor.c drivers/intel_gpu_service.c drivers/intel_gpu_fences.c kernel/sched_signal.c \
            drivers/intel_gpu_contexts.c drivers/intel_gpu_kmt_context.c \
            drivers/intel_gpu_cpu_view.c drivers/intel_gpu_kmt.c \
            drivers/intel_gpu_buffer.c drivers/intel_gpu_memory.c drivers/intel_bcs_context.c drivers/intel_rcs_sampled.c drivers/intel_rcs_vertex.c drivers/intel_gen9_texture.c drivers/intel_rcs.c drivers/intel_rcs_draw.c \
            drivers/intel_ppgtt.c drivers/intel_ggtt_space.c drivers/intel_bcs.c drivers/intel_cs.c \
            drivers/intel_bcs_queue.c win32/d3dkmt_registry.c -o "$out/draw-$cc-$mode"
        timeout 60 "$out/draw-$cc-$mode" "$program" | tee "$out/$cc-$mode.log"
    done
done
