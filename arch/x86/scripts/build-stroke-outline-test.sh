#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required}
for bits in 32 64; do
    dir="$out/pe$bits"; mkdir -p "$dir"
    if [[ $bits == 32 ]]; then
        target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
        cp arch/x86/test/wgl-backend-kernel32.def "$dir/kernel32.def"
    else
        target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64
        sed 's/@[0-9][0-9]*$//' arch/x86/test/wgl-backend-kernel32.def > "$dir/kernel32.def"
    fi
    if [[ $bits == 32 ]]; then echo 'GetCommandLineA@0' >> "$dir/kernel32.def";
    else echo 'GetCommandLineA' >> "$dir/kernel32.def"; fi
    llvm-dlltool -m "$dll_machine" -k -d "$dir/kernel32.def" -l "$dir/kernel32.lib"
    clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector -mno-stack-arg-probe -Wall -Wextra -Werror \
        -c arch/x86/test/stroke_outline_pe.c -o "$dir/outline.obj"
    lld-link /entry:stroke_outline_entry /subsystem:console /nodefaultlib /machine:"$machine" \
        /fixed "/out:$dir/outline$bits.exe" "$dir/outline.obj" "$dir/kernel32.lib"
done
