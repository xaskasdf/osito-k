#!/bin/bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$repo"
out=${1:-arch/x86/build/boot-handoff-host}
test ! -e "$out"
mkdir -p "$out"
cp --parents "$0" arch/x86/test/boot_handoff_host.c arch/x86/boot/boot_efi.c \
    arch/x86/boot/boot_paging.h arch/x86/include/boot_trace.h "$out/"
for compiler in gcc clang; do
    for variant in normal sanitized; do
        extra=()
        if [[ $variant == sanitized ]]; then
            extra=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        fi
        "$compiler" -std=c11 -O2 -Wall -Wextra -Werror -fshort-wchar \
            -DGNU_EFI_USE_MS_ABI -I/usr/include/efi -I/usr/include/efi/x86_64 \
            -ffunction-sections -fdata-sections "${extra[@]}" \
            arch/x86/test/boot_handoff_host.c -Wl,--gc-sections -o "$out/$compiler-$variant"
        "$out/$compiler-$variant" | tee "$out/$compiler-$variant.log"
    done
done
