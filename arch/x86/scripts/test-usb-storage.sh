#!/bin/bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$repo"
out=${1:-arch/x86/build/usb-storage-host}
test ! -e "$out"
mkdir -p "$out"
cp --parents "$0" arch/x86/test/usb_storage_host.c arch/x86/drivers/usb_storage.c "$out/"
for compiler in gcc clang; do
    for mode in normal sanitize; do
        extra=()
        if [ "$mode" = sanitize ]; then
            extra=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        fi
        "$compiler" -std=c11 -O2 -Wall -Wextra -Werror -pthread "${extra[@]}" \
            arch/x86/test/usb_storage_host.c -o "$out/$compiler-$mode"
        "$out/$compiler-$mode" | tee "$out/$compiler-$mode.log"
    done
done
