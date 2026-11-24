#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$repo"
out=${1:?output directory required}
mkdir -p "$out"
for bits in 32 64; do
    if [[ $bits == 32 ]]; then
        target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
        cp arch/x86/test/wgl-backend-kernel32.def "$out/kernel32.def"
    else
        target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64
        sed 's/@[0-9][0-9]*$//' arch/x86/test/wgl-backend-kernel32.def > "$out/kernel32.def"
    fi
    llvm-dlltool -m "$dll_machine" -k -d "$out/kernel32.def" -l "$out/kernel32.lib"
    clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
        -c arch/x86/test/user_object_pe.c -o "$out/userobject$bits.obj"
    lld-link /entry:mainCRTStartup /subsystem:console /nodefaultlib /machine:"$machine" /fixed \
        "/out:$out/userobject$bits.exe" "$out/userobject$bits.obj" "$out/kernel32.lib"
done
