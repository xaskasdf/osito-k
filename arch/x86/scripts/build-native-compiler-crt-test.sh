#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:-arch/x86/build/native-compiler-crt}
mkdir -p "$out"
for bits in 32 64; do
    target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
    if [[ $bits == 64 ]]; then target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64; fi
    clang --target="$target" -std=c11 -O2 -ffreestanding -fno-builtin -fno-stack-protector \
        -Wall -Wextra -Werror -c arch/x86/test/native_compiler_crt_pe.c -o "$out/probe$bits.obj"
    printf 'LIBRARY KERNEL32.dll\nEXPORTS\nGetStdHandle@4\nWriteFile@20\nLoadLibraryA@4\nGetProcAddress@8\nExitProcess@4\n' > "$out/kernel32-$bits.def"
    if [[ $bits == 64 ]]; then sed -i 's/@[0-9][0-9]*$//' "$out/kernel32-$bits.def"; fi
    llvm-dlltool -m "$dll_machine" -k -d "$out/kernel32-$bits.def" -l "$out/kernel32-$bits.lib"
    lld-link /entry:compiler_crt_entry /subsystem:console /nodefaultlib /machine:"$machine" /timestamp:0 /fixed \
        "/out:$out/compiler-crt$bits.exe" "$out/probe$bits.obj" "$out/kernel32-$bits.lib"
done
