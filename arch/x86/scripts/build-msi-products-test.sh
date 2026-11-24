#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required}
for bits in 32 64; do
    dir="$out/pe$bits"; mkdir -p "$dir"
    if [[ $bits == 32 ]]; then
        target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
    else
        target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64
    fi
    printf 'LIBRARY kernel32.dll\nEXPORTS\nExitProcess@4\nGetStdHandle@4\nWriteFile@20\nGetCommandLineA@0\n' > "$dir/kernel32.def"
    printf 'LIBRARY msi.dll\nEXPORTS\nMsiGetProductInfoW@16 @70 NONAME\n' > "$dir/msi.def"
    for dll in kernel32 msi; do
        if [[ $bits == 64 ]]; then
            sed -i -E 's/@[0-9]+( |$)/\1/;s/ @[0-9]+ NONAME//' "$dir/$dll.def"
        fi
        llvm-dlltool -m "$dll_machine" -k -d "$dir/$dll.def" -l "$dir/$dll.lib"
    done
    clang --target="$target" -std=c11 -O2 -ffreestanding -fno-builtin -fno-stack-protector \
        -Wall -Wextra -Werror -c arch/x86/test/msi_products_pe.c -o "$dir/probe.obj"
    lld-link /entry:msi_products_entry /subsystem:console /nodefaultlib /machine:"$machine" \
        /fixed "/out:$dir/msiproduct$bits.exe" "$dir/probe.obj" "$dir/kernel32.lib" "$dir/msi.lib"
done
