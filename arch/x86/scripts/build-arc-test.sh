#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required}
flags=()
if [[ ${ARC_PATH_REFERENCE:-0} == 1 ]]; then flags+=(-DARC_PATH_REFERENCE); fi
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
    clang "${flags[@]}" --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector -mno-stack-arg-probe -Wall -Wextra -Werror \
        -c arch/x86/test/arc_pe.c -o "$dir/arc.obj"
    lld-link /entry:arc_entry /subsystem:console /nodefaultlib /machine:"$machine" \
        /fixed "/out:$dir/arc$bits.exe" "$dir/arc.obj" "$dir/kernel32.lib"
done
