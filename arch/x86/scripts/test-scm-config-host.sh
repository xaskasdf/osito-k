#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required}; mkdir -p "$out"
for cc in gcc clang; do
    for mode in normal sanitize; do
        flags=()
        [[ $mode == sanitize ]] && flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        "$cc" -O1 -g -ffunction-sections -fdata-sections -Iinclude/common \
            -D__KERNEL_X86__ -Iarch/x86/include \
            -Wno-unused-function "${flags[@]}" arch/x86/test/scm_config_host.c \
            -Wl,--gc-sections -o "$out/scm-$cc-$mode"
        "$out/scm-$cc-$mode"
    done
done
