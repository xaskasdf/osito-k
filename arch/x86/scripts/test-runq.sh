#!/bin/bash
set -euo pipefail
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1
repo=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$repo"
out=${1:-arch/x86/build/runq-host}
test ! -e "$out"
mkdir -p "$out"
cp --parents "$0" arch/x86/test/runq_host.c arch/x86/kernel/{process.c,smp.h,smp_work.h,cpu_idle.h,sched_signal.c,sched_signal.h} \
    arch/x86/include/{types.h,stdint.h,fd.h,paging.h,interrupt.h,proc_cycles.h,sys_caps.h} \
    arch/x86/fs/vfs.h "$out/"
if [[ -n ${OSITO_RUNQ_TEST_SOURCE:-} ]]; then
    cp "$OSITO_RUNQ_TEST_SOURCE" "$out/arch/x86/kernel/process.c"
fi
out=$(realpath "$out")
cd "$out"
failed=0
for compiler in gcc clang; do
    for mode in normal sanitize; do
        flags=()
        if [[ $mode == sanitize ]]; then
            flags=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        fi
        "$compiler" -std=c11 -O2 -g -Wall -Wextra -Werror -ffreestanding -fno-builtin \
            -Iarch/x86/include "${flags[@]}" \
            -ffunction-sections -fdata-sections -Wl,--gc-sections \
            arch/x86/test/runq_host.c arch/x86/kernel/sched_signal.c -o "$out/$compiler-$mode"
        for test in normal preempt ineligible irq-state churn signal; do
            "$out/$compiler-$mode" "$test" | tee "$out/$compiler-$mode-$test.log" || failed=1
        done
    done
done
exit "$failed"
