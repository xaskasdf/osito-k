#!/bin/bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$repo"
out=${1:-arch/x86/build/ps2-host}
test ! -e "$out"
mkdir -p "$out"
cp --parents "$0" arch/x86/drivers/ps2.c arch/x86/drivers/ps2.h \
    arch/x86/include/types.h arch/x86/test/ps2_host.c "$out/"
for compiler in gcc clang; do
    for mode in normal sanitize; do
        flags=()
        if [[ $mode == sanitize ]]; then
            flags=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        fi
        "$compiler" -std=c11 -O2 -g -Wall -Wextra -Werror \
            "${flags[@]}" arch/x86/test/ps2_host.c -o "$out/$compiler-$mode"
        "$out/$compiler-$mode" | tee "$out/$compiler-$mode.log"
    done
done
