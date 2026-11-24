#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-build/screen-capture-host}
mkdir -p "$out"
flags=(-std=c11 -O2 -g -Wall -Wextra -DSCREEN_CAPTURE_UNIT_TEST -ffunction-sections -fdata-sections)
if [[ ${SANITIZE:-0} == 1 ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
"${CC:-cc}" "${flags[@]}" kernel/screen_capture.c kernel/zlib.c kernel/crypto.c test/screen_capture_host.c \
    -Wl,--gc-sections -o "$out/screen-capture-host"
"$out/screen-capture-host"
