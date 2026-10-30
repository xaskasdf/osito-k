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
    printf 'LIBRARY kernel32.dll\nEXPORTS\nExitProcess@4\nGetStdHandle@4\nWriteFile@20\nGetModuleHandleA@4\nGetProcAddress@8\nSetLastError@4\nGetLastError@0\n' > "$dir/kernel32.def"
    printf 'LIBRARY user32.dll\nEXPORTS\nRegisterClassA@4\nUnregisterClassA@8\nCreateWindowExA@48\nDestroyWindow@4\nDefWindowProcA@16\n' > "$dir/user32.def"
    for dll in kernel32 user32; do
        if [[ $bits == 64 ]]; then sed -i 's/@[0-9][0-9]*$//' "$dir/$dll.def"; fi
        llvm-dlltool -m "$dll_machine" -k -d "$dir/$dll.def" -l "$dir/$dll.lib"
    done
    clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
        -Wall -Wextra -Werror -c arch/x86/test/class_name_pe.c -o "$dir/probe.obj"
    lld-link /entry:class_name_entry /subsystem:console /nodefaultlib /machine:"$machine" \
        /fixed "/out:$dir/classname$bits.exe" "$dir/probe.obj" "$dir/kernel32.lib" "$dir/user32.lib"
done
