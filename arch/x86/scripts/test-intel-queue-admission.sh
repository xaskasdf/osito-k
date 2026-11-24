#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-build/intel-gpu-submit-host}
mkdir -p "$out"
ulimit -c 0
for cc in gcc clang; do
    for mode in normal sanitize; do
        flags=()
        if [[ $mode == sanitize ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
        for test in wakeup worker rcs rebind; do
            extra=()
            case "$test" in
                wakeup) source=test/intel_bcs_wakeup_host.c ;;
                worker) source=test/intel_bcs_worker_host.c; extra+=(drivers/intel_bcs_worker.c kernel/sched_signal.c) ;;
                rcs) source=test/intel_rcs_host.c; extra+=(drivers/intel_rcs.c) ;;
                rebind) source=test/intel_bcs_rebind_host.c; extra+=(drivers/intel_bcs_context.c drivers/intel_rcs_sampled.c drivers/intel_rcs_vertex.c drivers/intel_gen9_texture.c drivers/intel_gen9_shader.c drivers/intel_rcs.c drivers/intel_rcs_draw.c drivers/intel_ppgtt.c drivers/intel_ggtt_space.c) ;;
            esac
            "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${flags[@]}" \
                "$source" "${extra[@]}" drivers/intel_bcs.c drivers/intel_cs.c drivers/intel_bcs_queue.c \
                win32/d3dkmt_registry.c drivers/intel_gpu_memory.c drivers/intel_gpu_buffer.c \
                -o "$out/$test-$cc-$mode"
            timeout 60 "$out/$test-$cc-$mode" | tee "$out/$test-$cc-$mode.log"
        done
    done
done
