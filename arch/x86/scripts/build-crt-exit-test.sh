#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:-arch/x86/build/crt-startup}
mkdir -p "$out"
for bits in 32 64; do
    target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
    if [[ $bits == 64 ]]; then target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64; fi
    clang --target="$target" -std=c11 -O2 -ffreestanding -fno-builtin -fno-stack-protector \
        -Wall -Wextra -Werror -c arch/x86/test/crt_exit_pe.c -o "$out/crt_exit$bits.obj"
    printf 'LIBRARY KERNEL32.dll\nEXPORTS\nGetStdHandle@4\nWriteFile@20\nLoadLibraryA@4\nGetProcAddress@8\nExitProcess@4\n' > "$out/crt-exit-kernel32-$bits.def"
    if [[ $bits == 64 ]]; then sed -i 's/@[0-9][0-9]*$//' "$out/crt-exit-kernel32-$bits.def"; fi
    llvm-dlltool -m "$dll_machine" -k -d "$out/crt-exit-kernel32-$bits.def" -l "$out/crt-exit-kernel32-$bits.lib"
    lld-link /entry:crt_exit_test_entry /subsystem:console /nodefaultlib /machine:"$machine" /timestamp:0 /fixed \
        "/out:$out/crt-exit$bits.exe" "$out/crt_exit$bits.obj" "$out/crt-exit-kernel32-$bits.lib"
done
