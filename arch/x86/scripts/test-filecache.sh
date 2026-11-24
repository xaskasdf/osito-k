#!/bin/bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$repo"
out=${1:-arch/x86/build/filecache-host}
test ! -e "$out"
mkdir -p "$out"
cp --parents "$0" arch/x86/test/filecache_host.c arch/x86/win32/filecache.c \
    arch/x86/win32/filecache.h arch/x86/win32/handle.h arch/x86/win32/nttypes.h "$out/"
for compiler in gcc clang; do
    for mode in normal sanitize; do
        extra=()
        if [ "$mode" = sanitize ]; then
            extra=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        fi
        "$compiler" -std=c11 -O2 -Wall -Wextra -Werror -fno-builtin -pthread \
            -iquote arch/x86/include "${extra[@]}" \
            arch/x86/test/filecache_host.c -o "$out/$compiler-$mode"
        "$out/$compiler-$mode" | tee "$out/$compiler-$mode.log"
    done
done
