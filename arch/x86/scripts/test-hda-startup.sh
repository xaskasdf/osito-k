#!/bin/bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$repo"
out=${1:-arch/x86/build/hda-startup-host}
test ! -e "$out"
mkdir -p "$out"
cp --parents "$0" arch/x86/test/hda_startup_host.c arch/x86/drivers/hda.c \
    arch/x86/drivers/hda.h arch/x86/include/{types,paging,pci}.h "$out/"
for compiler in gcc clang; do
    for mode in normal sanitize; do
        extra=()
        if [ "$mode" = sanitize ]; then
            extra=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        fi
        "$compiler" -std=c11 -O2 -Wall -Wextra -Werror -Wno-sign-compare -ffunction-sections \
            -fdata-sections "${extra[@]}" -Iarch/x86/include -Iinclude/common \
            arch/x86/test/hda_startup_host.c -Wl,--gc-sections \
            -o "$out/hda-$compiler-$mode"
        "$out/hda-$compiler-$mode" | tee "$out/hda-$compiler-$mode.log"
    done
done
