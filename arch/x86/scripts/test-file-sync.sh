#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-build/file-sync-host}
mkdir -p "$out"
cc=${CC:-cc}
flags=(-std=c11 -O2 -g -Wall -Wextra -ffunction-sections -fdata-sections)
if [[ ${SANITIZE:-0} == 1 ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
"$cc" "${flags[@]}" -DFILE_SYNC_STDIO test/file_sync_host.c kernel/file_sync.c kernel/crypto.c \
    -Wl,--gc-sections -o "$out/file-sync-host"
python3 ../../tools/test-osito-sync.py "$out/file-sync-host"
