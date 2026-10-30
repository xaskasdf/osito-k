#!/bin/bash
set -euo pipefail
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1
repo=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$repo"
out=${1:-arch/x86/build/virtio-net-host}
test ! -e "$out"
mkdir -p "$out"
cp --parents "$0" arch/x86/test/virtio_net_host.c \
    arch/x86/drivers/{virtio.c,virtio_net.c} arch/x86/kernel/smp.h \
    arch/x86/include/{types.h,paging.h} "$out/"
out=$(realpath "$out")
cd "$out"
for compiler in gcc clang; do
    for mode in normal sanitize; do
        flags=()
        if [[ $mode == sanitize ]]; then
            flags=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        fi
        "$compiler" -std=c11 -O2 -g -Wall -Wextra -Werror "${flags[@]}" \
            -pthread -ffunction-sections -fdata-sections -Wl,--gc-sections \
            arch/x86/test/virtio_net_host.c arch/x86/drivers/virtio.c \
            -o "$out/$compiler-$mode"
        for test in queue owned full concurrent reset size; do
            "$out/$compiler-$mode" "$test" | tee "$out/$compiler-$mode-$test.log"
        done
    done
done
