#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-build/kernel-update-host}
mkdir -p "$out"
for cc in gcc clang; do
    for mode in normal sanitize; do
        flags=()
        if [[ $mode == sanitize ]]; then
            flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        fi
        "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror "${flags[@]}" \
            test/boot_kernel_host.c -o "$out/boot-kernel-$cc-$mode"
        "$out/boot-kernel-$cc-$mode"
        "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror "${flags[@]}" \
            test/kernel_update_host.c -o "$out/kernel-update-$cc-$mode"
        "$out/kernel-update-$cc-$mode"
    done
done
