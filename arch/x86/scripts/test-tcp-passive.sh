#!/bin/bash
set -euo pipefail
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1
repo=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$repo"
out=${1:-arch/x86/build/tcp-passive-host}
test ! -e "$out"
mkdir -p "$out"
cp --parents "$0" arch/x86/test/tcp_passive_host.c arch/x86/kernel/{net.c,net.h,nic.h,smp.h} \
    arch/x86/include/{types.h,paging.h} arch/x86/drivers/i211.h "$out/"
if [[ -n ${OSITO_TCP_TEST_NET_SOURCE:-} ]]; then
    cp "$OSITO_TCP_TEST_NET_SOURCE" "$out/arch/x86/kernel/net.c"
fi
out=$(realpath "$out")
cd "$out"
for compiler in gcc clang; do
    for mode in normal sanitize; do
        flags=()
        if [[ $mode == sanitize ]]; then
            flags=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        fi
        "$compiler" -std=c11 -O2 -g -Wall -Wextra -Werror "${flags[@]}" \
            -ffunction-sections -fdata-sections -Wl,--gc-sections \
            arch/x86/test/tcp_passive_host.c -o "$out/$compiler-$mode"
        "$out/$compiler-$mode" | tee "$out/$compiler-$mode.log"
    done
done
