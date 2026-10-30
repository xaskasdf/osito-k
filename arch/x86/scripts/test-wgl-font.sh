#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-build/wgl-font-host}
mkdir -p "$out"
flags=(-std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-address
       -ffunction-sections -fdata-sections -Iinclude -Iwin32 -Ikernel
       -I../../include/common -Ilib/opengl/mesa/include -D__KERNEL_X86__=1)
if [[ ${SANITIZE:-0} == 1 ]]; then
    flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    # Retain section GC for unrelated kernel export tables. Heap/stack accesses,
    # including the glyph output and pixel comparison buffers, stay instrumented.
    if [[ ${CC:-cc} == *clang* ]]; then
        flags+=(-mllvm -asan-globals=0)
    else
        flags+=(--param asan-globals=0)
    fi
fi
"${CC:-cc}" "${flags[@]}" test/wgl_font_host.c ../../gui/gui_text.c \
    -Wl,--gc-sections -lEGL -lGL -ldl -o "$out/wgl-font-host"
EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 "$out/wgl-font-host"
