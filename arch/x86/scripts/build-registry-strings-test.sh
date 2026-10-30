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
    printf 'LIBRARY kernel32.dll\nEXPORTS\nExitProcess@4\nGetStdHandle@4\nWriteFile@20\nGetCurrentProcessId@0\nSetLastError@4\nGetLastError@0\n' > "$dir/kernel32.def"
    printf 'LIBRARY advapi32.dll\nEXPORTS\nRegCreateKeyExA@36\nRegSetValueExA@24\nRegSetValueExW@24\nRegQueryValueExA@24\nRegQueryValueExW@24\nRegEnumValueA@32\nRegEnumValueW@32\nRegQueryInfoKeyA@48\nRegQueryInfoKeyW@48\nRegDeleteTreeA@8\nRegDeleteKeyA@8\nRegCloseKey@4\n' > "$dir/advapi32.def"
    for dll in kernel32 advapi32; do
        if [[ $bits == 64 ]]; then sed -i 's/@[0-9][0-9]*$//' "$dir/$dll.def"; fi
        llvm-dlltool -m "$dll_machine" -k -d "$dir/$dll.def" -l "$dir/$dll.lib"
    done
    clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
        -Wall -Wextra -Werror -c arch/x86/test/registry_strings_pe.c -o "$dir/probe.obj"
    lld-link /entry:registry_strings_entry /subsystem:console /nodefaultlib /machine:"$machine" \
        /fixed "/out:$dir/regstring$bits.exe" "$dir/probe.obj" "$dir/kernel32.lib" "$dir/advapi32.lib"
done
