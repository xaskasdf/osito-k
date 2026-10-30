#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$repo"
out=${1:?output directory required}
mkdir -p "$out"
for bits in 32 64; do
    if [[ $bits == 32 ]]; then
        target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
    else
        target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64
    fi
    for dll in kernel32 ntdll; do
        def=arch/x86/test/shim-exports-ntdll.def
        [[ $dll == kernel32 ]] && def=arch/x86/test/wgl-backend-kernel32.def
        if [[ $bits == 32 ]]; then
            cp "$def" "$out/$dll.def"
        else
            sed 's/@[0-9][0-9]*$//' "$def" > "$out/$dll.def"
        fi
        llvm-dlltool -m "$dll_machine" -k -d "$out/$dll.def" -l "$out/$dll.lib"
    done
    clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
        -c arch/x86/test/shim_exports_pe.c -o "$out/shimexports$bits.obj"
    lld-link /entry:mainCRTStartup /subsystem:console /nodefaultlib /machine:"$machine" /fixed \
        "/out:$out/shimexports$bits.exe" "$out/shimexports$bits.obj" "$out/kernel32.lib" "$out/ntdll.lib"
done
