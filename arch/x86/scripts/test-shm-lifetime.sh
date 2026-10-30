#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:-build/shm-lifetime-host}
mkdir -p "$out"
flags=(-std=c11 -O2 -g -Wall -Wextra -pthread -ffunction-sections -fdata-sections
       -iquote arch/x86/kernel)
for compiler in gcc clang; do
    for mode in normal sanitize; do
        extra=()
        if [[ $mode == sanitize ]]; then
            extra+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
        fi
        "$compiler" "${flags[@]}" "${extra[@]}" arch/x86/test/shm_lifetime_host.c \
            -Wl,--gc-sections -o "$out/$compiler-$mode"
        timeout 60 "$out/$compiler-$mode" | tee "$out/$compiler-$mode.log"
    done
done
