#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:-build/win32-present-surface}
mkdir -p "$out"
for api in ddraw wgl; do
    for compiler in gcc clang; do
        for mode in normal sanitize; do
            flags=(-std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-address
                   -ffunction-sections -fdata-sections -Iarch/x86/include
                   -Iarch/x86/win32 -Iarch/x86/kernel -Iinclude/common
                   -Iarch/x86/lib/opengl/mesa/include)
            if [[ $api == wgl ]]; then flags+=(-DTEST_WGL); fi
            if [[ $mode == sanitize ]]; then
                flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
            fi
            if [[ -n ${PRESENT_TEST_SOURCE_DIR:-} ]]; then
                source=ddraw_shim.c
                if [[ $api == wgl ]]; then source=opengl32_shim.c; fi
                flags+=("-DPRESENT_TEST_SOURCE=\"$(realpath "$PRESENT_TEST_SOURCE_DIR/$source")\"")
            fi
            binary="$out/$api-$compiler-$mode"
            "$compiler" "${flags[@]}" arch/x86/test/win32_present_surface_host.c \
                -Wl,--gc-sections -o "$binary"
            timeout 60 "$binary" 2>&1 | tee "$binary.log"
        done
    done
done
