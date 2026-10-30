#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required}
GPUCOPY_MODES=copy bash arch/x86/scripts/build-native-gpu-copy.sh "$out"
for bits in 32 64; do
    dir="$out/pe$bits";target=i686-pc-windows-msvc;machine=x86
    [[ $bits == 64 ]] && target=x86_64-pc-windows-msvc && machine=x64
    clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
        -Wall -Wextra -Werror -c arch/x86/test/native_gpu_rect_pe.c -o "$dir/rect.obj"
    lld-link /entry:native_gpu_rect_entry /subsystem:console /nodefaultlib /machine:"$machine" \
        /fixed /timestamp:0 "/out:$dir/gpurect$bits.exe" "$dir/rect.obj" "$dir/kernel32.lib"
done
