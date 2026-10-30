#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-build/intel-ggtt-host}
mkdir -p "$out"
ulimit -c 0
for cc in gcc clang; do
    for mode in normal sanitize; do
        flags=()
        if [[ $mode == sanitize ]]; then
            flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        fi
        "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror "${flags[@]}" \
            test/intel_ggtt_host.c drivers/intel_ggtt.c -o "$out/ggtt-$cc-$mode"
        "$out/ggtt-$cc-$mode" | tee "$out/$cc-$mode.log"
        "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror -fno-builtin "${flags[@]}" \
            -D__KERNEL_X86__=1 test/intel_gfx_probe_host.c drivers/intel_ggtt.c \
            -o "$out/probe-$cc-$mode"
        "$out/probe-$cc-$mode" | tee "$out/probe-$cc-$mode.log"
        "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${flags[@]}" \
            test/intel_ggtt_space_host.c drivers/intel_ggtt_space.c \
            -o "$out/space-$cc-$mode"
        "$out/space-$cc-$mode" | tee "$out/space-$cc-$mode.log"
        "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror "${flags[@]}" \
            test/intel_gt_host.c drivers/intel_gt.c -o "$out/gt-$cc-$mode"
        "$out/gt-$cc-$mode" | tee "$out/gt-$cc-$mode.log"
        "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${flags[@]}" \
            test/intel_bcs_host.c drivers/intel_bcs.c drivers/intel_cs.c drivers/intel_bcs_queue.c \
            win32/d3dkmt_registry.c drivers/intel_gpu_memory.c drivers/intel_gpu_buffer.c \
            -o "$out/bcs-$cc-$mode"
        "$out/bcs-$cc-$mode" | tee "$out/bcs-$cc-$mode.log"
        "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${flags[@]}" \
            test/intel_rcs_host.c drivers/intel_rcs.c drivers/intel_cs.c drivers/intel_bcs.c \
            drivers/intel_bcs_queue.c win32/d3dkmt_registry.c drivers/intel_gpu_memory.c \
            drivers/intel_gpu_buffer.c -o "$out/rcs-$cc-$mode"
        "$out/rcs-$cc-$mode" | tee "$out/rcs-$cc-$mode.log"
        "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${flags[@]}" \
            test/intel_bcs_wakeup_host.c drivers/intel_bcs.c drivers/intel_cs.c drivers/intel_bcs_queue.c \
            win32/d3dkmt_registry.c drivers/intel_gpu_memory.c drivers/intel_gpu_buffer.c \
            -o "$out/wakeup-$cc-$mode"
        timeout 30 "$out/wakeup-$cc-$mode" | tee "$out/wakeup-$cc-$mode.log"
        "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${flags[@]}" \
            test/intel_bcs_worker_host.c drivers/intel_bcs_worker.c kernel/sched_signal.c \
            drivers/intel_bcs.c drivers/intel_cs.c drivers/intel_bcs_queue.c win32/d3dkmt_registry.c \
            drivers/intel_gpu_memory.c drivers/intel_gpu_buffer.c -o "$out/worker-$cc-$mode"
        timeout 30 "$out/worker-$cc-$mode" | tee "$out/worker-$cc-$mode.log"
        "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror "${flags[@]}" \
            test/intel_bcs_irq_host.c drivers/intel_bcs_irq.c -o "$out/irq-$cc-$mode"
        "$out/irq-$cc-$mode" | tee "$out/irq-$cc-$mode.log"
        "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror "${flags[@]}" \
            test/intel_ppgtt_host.c drivers/intel_ppgtt.c -o "$out/ppgtt-$cc-$mode"
        "$out/ppgtt-$cc-$mode" | tee "$out/ppgtt-$cc-$mode.log"
        "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${flags[@]}" \
            test/intel_gpu_resource_host.c drivers/intel_gpu_buffer.c \
            drivers/intel_bcs_context.c drivers/intel_rcs_sampled.c drivers/intel_rcs_vertex.c drivers/intel_gen9_texture.c drivers/intel_gen9_shader.c drivers/intel_rcs.c drivers/intel_rcs_draw.c drivers/intel_ppgtt.c drivers/intel_ggtt_space.c \
            drivers/intel_bcs.c drivers/intel_cs.c drivers/intel_bcs_queue.c drivers/intel_gpu_memory.c \
            win32/d3dkmt_registry.c -o "$out/resource-$cc-$mode"
        "$out/resource-$cc-$mode" | tee "$out/resource-$cc-$mode.log"
        "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${flags[@]}" \
            test/intel_bcs_rebind_host.c drivers/intel_gpu_buffer.c \
            drivers/intel_bcs_context.c drivers/intel_rcs_sampled.c drivers/intel_rcs_vertex.c drivers/intel_gen9_texture.c drivers/intel_gen9_shader.c drivers/intel_rcs.c drivers/intel_rcs_draw.c drivers/intel_ppgtt.c drivers/intel_ggtt_space.c \
            drivers/intel_bcs.c drivers/intel_cs.c drivers/intel_bcs_queue.c drivers/intel_gpu_memory.c \
            win32/d3dkmt_registry.c -o "$out/rebind-$cc-$mode"
        "$out/rebind-$cc-$mode" | tee "$out/rebind-$cc-$mode.log"
        "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror -fno-builtin \
            -ffunction-sections -fdata-sections -Wl,--gc-sections -Iinclude "${flags[@]}" \
            test/intel_ggtt_window_host.c drivers/intel_ggtt.c drivers/intel_bcs.c drivers/intel_cs.c \
            drivers/intel_bcs_context.c drivers/intel_rcs_sampled.c drivers/intel_rcs_vertex.c drivers/intel_gen9_texture.c drivers/intel_gen9_shader.c drivers/intel_rcs.c drivers/intel_rcs_draw.c drivers/intel_gpu_buffer.c drivers/intel_ppgtt.c \
            drivers/intel_ggtt_space.c drivers/intel_bcs_queue.c \
            -o "$out/window-$cc-$mode"
        "$out/window-$cc-$mode" | tee "$out/window-$cc-$mode.log"
    done
done
bash scripts/test-intel-rcs-draw.sh "$out"
bash scripts/test-intel-gpu-kmt.sh "$out/kmt-provider"
