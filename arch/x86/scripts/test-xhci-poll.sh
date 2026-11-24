#!/bin/bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$repo"
out=${1:-arch/x86/build/xhci-poll-host}
test ! -e "$out"
mkdir -p "$out"
cp --parents "$0" arch/x86/test/xhci_poll_host.c arch/x86/drivers/xhci.c \
    arch/x86/drivers/xhci.h arch/x86/drivers/hid_parser.c \
    arch/x86/drivers/hid_parser.h arch/x86/test/boot_storage_host.c \
    arch/x86/kernel/boot_diag.c "$out/"
for compiler in gcc clang; do
    for mode in normal sanitize; do
        extra=()
        if [ "$mode" = sanitize ]; then
            extra=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        fi
        for test in xhci_poll boot_storage; do
            "$compiler" -std=c11 -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
                "${extra[@]}" -Iarch/x86/include -Iinclude/common "arch/x86/test/${test}_host.c" \
                arch/x86/drivers/hid_parser.c -Wl,--gc-sections -o "$out/$test-$compiler-$mode"
            "$out/$test-$compiler-$mode" | tee "$out/$test-$compiler-$mode.log"
        done
    done
done
