#!/bin/bash
set -euo pipefail
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1
repo=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$repo"
out=${1:-arch/x86/build/dhcp-host}
test ! -e "$out"
mkdir -p "$out"
cp --parents "$0" arch/x86/test/{dhcp,apipa}_host.c arch/x86/kernel/{dhcp.c,apipa.c,net.h} \
    arch/x86/include/types.h "$out/"
out=$(realpath "$out")
cd "$out"
for compiler in gcc clang; do
    for mode in normal sanitize; do
        flags=()
        if [[ $mode == sanitize ]]; then
            flags=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        fi
        for suite in dhcp apipa; do
            "$compiler" -std=c11 -O2 -g -Wall -Wextra -Werror "${flags[@]}" \
                "arch/x86/test/${suite}_host.c" -o "$out/$suite-$compiler-$mode"
            "$out/$suite-$compiler-$mode" | tee "$out/$suite-$compiler-$mode.log"
        done
    done
done
