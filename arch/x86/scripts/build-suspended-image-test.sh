#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$repo"
out=${1:?output directory required}
mkdir -p "$out"
for bits in 32 64; do
    if [[ $bits == 32 ]]; then
        target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
        tls=__tls_used; notify=NotifyAttach=_NotifyAttach@0
        cp arch/x86/test/suspended-image-kernel32.def "$out/kernel32.def"
    else
        target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64
        tls=_tls_used; notify=NotifyAttach
        sed 's/@[0-9][0-9]*$//' arch/x86/test/suspended-image-kernel32.def > "$out/kernel32.def"
    fi
    llvm-dlltool -m "$dll_machine" -k -d "$out/kernel32.def" -l "$out/kernel32.lib"
    clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
        -DDEPENDENCY -c arch/x86/test/suspended_image_pe.c -o "$out/dependency$bits.obj"
    lld-link /dll /entry:DllMain /nodefaultlib /machine:"$machine" \
        "/out:$out/suspendeddep$bits.dll" "/implib:$out/dependency$bits.lib" \
        "$out/dependency$bits.obj" "$out/kernel32.lib"
    clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
        -c arch/x86/test/suspended_image_pe.c -o "$out/suspended$bits.obj"
    lld-link /entry:mainCRTStartup /subsystem:console /nodefaultlib /machine:"$machine" /fixed \
        /include:"$tls" /export:"$notify" "/out:$out/suspended$bits.exe" \
        "$out/suspended$bits.obj" "$out/kernel32.lib" "$out/dependency$bits.lib"
done
