#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required}
bash arch/x86/scripts/build-native-d3dkmt-test.sh "$out"
for bits in 32 64; do
    dir="$out/pe$bits"
    if [[ $bits == 32 ]]; then target=i686-pc-windows-msvc; machine=x86;
    else target=x86_64-pc-windows-msvc; machine=x64; fi
    for mode in device empty; do
        flags=(); if [[ $mode == empty ]]; then flags+=(-DKMT_SYNC_EMPTY); fi
        clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
            -Wall -Wextra -Werror "${flags[@]}" -c arch/x86/test/native_kmt_sync_pe.c -o "$dir/sync-$mode.obj"
        lld-link /entry:native_kmt_sync_entry /subsystem:console /nodefaultlib \
            /machine:"$machine" /fixed /timestamp:0 "/out:$dir/sync$bits-$mode.exe" \
            "$dir/sync-$mode.obj" "$dir/kernel32.lib"
    done
done
