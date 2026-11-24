#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-build/ssh-host}
mkdir -p "$out/obj"
cc=${CC:-cc}
flags=(-std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter -fwrapv -DOSITO_SSH_HOST_TEST)
if [[ ${FILE_SYNC_TEST:-0} == 1 ]]; then flags+=(-DOSITO_SYNC_TEST -ffunction-sections -fdata-sections); fi
if [[ ${SCREEN_CAPTURE_TEST:-0} == 1 ]]; then flags+=(-DOSITO_SCREEN_TEST -ffunction-sections -fdata-sections); fi
if [[ ${SANITIZE:-0} == 1 ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
objects=()
for src in lib/tinyssh/upstream/*.c lib/tinyssh/osito_port.c; do
    obj="$out/obj/$(basename "${src%.c}").o"
    "$cc" "${flags[@]}" -Ilib/tinyssh/port -Ilib/tinyssh/upstream -Ilib/tinyssh/upstream/cryptoint -c "$src" -o "$obj"
    objects+=("$obj")
done
if [[ ${FILE_SYNC_TEST:-0} == 1 ]]; then
    for src in kernel/file_sync.c kernel/crypto.c test/file_sync_host.c; do
        obj="$out/obj/$(basename "${src%.c}").o"
        "$cc" "${flags[@]}" -c "$src" -o "$obj"
        objects+=("$obj")
    done
    flags+=(-Wl,--gc-sections)
fi
if [[ ${SCREEN_CAPTURE_TEST:-0} == 1 ]]; then
    sources=(kernel/screen_capture.c kernel/zlib.c test/screen_capture_host.c)
    if [[ ${FILE_SYNC_TEST:-0} != 1 ]]; then sources+=(kernel/crypto.c); fi
    for src in "${sources[@]}"; do
        obj="$out/obj/$(basename "${src%.c}").o"
        "$cc" "${flags[@]}" -c "$src" -o "$obj"
        objects+=("$obj")
    done
    flags+=(-Wl,--gc-sections)
fi
"$cc" "${flags[@]}" test/ssh_host.c "${objects[@]}" -o "$out/ssh-host"
