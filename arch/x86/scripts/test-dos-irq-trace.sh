#!/bin/bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$repo"
out=${1:-arch/x86/build/dos-irq-trace-host}
test ! -e "$out"
mkdir -p "$out"
cp "$0" arch/x86/test/dos_irq_trace_host.c "$out/"
for compiler in gcc clang; do
    for mode in disabled enabled; do
        flags=()
        if [ "$mode" = enabled ]; then flags+=(-DDOS_IRQ_DIAGNOSTICS=1); fi
        "$compiler" -std=c11 -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
            -Iarch/x86/include -Iinclude/common "${flags[@]}" \
            arch/x86/test/dos_irq_trace_host.c -Wl,--gc-sections -o "$out/$compiler-$mode"
        "$out/$compiler-$mode" | tee "$out/$compiler-$mode.log"
        nm -a "$out/$compiler-$mode" > "$out/$compiler-$mode.nm"
        if [ "$mode" = disabled ]; then
            ! grep -q 'dos_.*irq_trace' "$out/$compiler-$mode.nm"
        else
            grep -q 'dos_irq_trace_records' "$out/$compiler-$mode.nm"
        fi
    done
done
