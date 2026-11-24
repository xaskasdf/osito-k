#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-build/gdi-screen-host}
mkdir -p "$out"
source=()
if [[ -n ${GDI_SCREEN_TEST_SOURCE:-} ]]; then
    source+=("-DGDI_SCREEN_TEST_SOURCE=\"$(realpath "$GDI_SCREEN_TEST_SOURCE")\"")
fi
flags=(-std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-address
       -ffunction-sections -fdata-sections -Iinclude -Iwin32 -Ikernel
       -I../../include/common)
if [[ ${SANITIZE:-0} == 1 ]]; then
    flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
fi
"${CC:-cc}" "${flags[@]}" "${source[@]}" test/gdi_screen_host.c \
    -Wl,--gc-sections -o "$out/gdi-screen-host"
"$out/gdi-screen-host"
