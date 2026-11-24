#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:-arch/x86/build/crt-startup}
mkdir -p "$out"
for bits in 32 64; do
    target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
    if [[ $bits == 64 ]]; then target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64; fi
    printf 'LIBRARY KERNEL32.dll\nEXPORTS\nGetStdHandle@4\nWriteFile@20\nLoadLibraryA@4\nGetProcAddress@8\nExitProcess@4\nGetLastError@0\nGetTickCount@0\nCreateThread@24\nWaitForSingleObject@8\nCloseHandle@4\nTerminateThread@8\nSleep@4\n' > "$out/address-kernel32-$bits.def"
    if [[ $bits == 64 ]]; then sed -i 's/@[0-9][0-9]*$//' "$out/address-kernel32-$bits.def"; fi
    llvm-dlltool -m "$dll_machine" -k -d "$out/address-kernel32-$bits.def" -l "$out/address-kernel32-$bits.lib"
    for variant in public native; do
        flags=()
        if [[ $variant == native ]]; then flags+=(-DADDRESS_WAIT_NATIVE_CANCEL=1); fi
        clang --target="$target" -std=c11 -O2 -ffreestanding -fno-builtin -fno-stack-protector \
            -Wall -Wextra -Werror "${flags[@]}" -c arch/x86/test/address_wait_pe.c -o "$out/address_wait$bits-$variant.obj"
        lld-link /entry:address_wait_entry /subsystem:console /nodefaultlib /machine:"$machine" /timestamp:0 /fixed \
            "/out:$out/address-wait$bits-$variant.exe" "$out/address_wait$bits-$variant.obj" "$out/address-kernel32-$bits.lib"
    done
done
