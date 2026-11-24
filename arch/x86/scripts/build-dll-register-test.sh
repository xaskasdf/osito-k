#!/usr/bin/env bash
# Compile only. These probes execute external DLLs only inside Osito-K guests.
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required}
for bits in 32 64; do
    dir="$out/pe$bits"; mkdir -p "$dir"
    cp arch/x86/test/wgl-backend-kernel32.def "$dir/kernel32.def"
    printf 'FreeLibrary@4\nGetCommandLineA@0\n' >> "$dir/kernel32.def"
    if [[ $bits == 32 ]]; then
        target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
    else
        target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64
        sed -i 's/@[0-9][0-9]*$//' "$dir/kernel32.def"
    fi
    llvm-dlltool -m "$dll_machine" -k -d "$dir/kernel32.def" -l "$dir/kernel32.lib"
    clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
        -Wall -Wextra -Werror -c arch/x86/test/dll_register_pe.c -o "$dir/probe.obj"
    lld-link /entry:dll_register_entry /subsystem:console /nodefaultlib /machine:"$machine" \
        /fixed "/out:$dir/dllregister$bits.exe" "$dir/probe.obj" "$dir/kernel32.lib"
done
