#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required}
for bits in 32 64; do
    dir="$out/pe$bits";mkdir -p "$dir"
    printf 'LIBRARY kernel32.dll\nEXPORTS\nExitProcess@4\nGetStdHandle@4\nWriteFile@20\nLoadLibraryA@4\nGetProcAddress@8\nCreateThread@24\nVirtualQuery@12\nVirtualAlloc@16\nVirtualProtect@16\nWaitForSingleObject@8\nGetExitCodeThread@8\nCloseHandle@4\nSetErrorMode@4\n' > "$dir/kernel32.def"
    if [[ $bits == 32 ]];then
        target=i686-pc-windows-msvc;machine=x86;dll_machine=i386
    else
        target=x86_64-pc-windows-msvc;machine=x64;dll_machine=i386:x86-64
        sed -i 's/@[0-9][0-9]*$//' "$dir/kernel32.def"
    fi
    llvm-dlltool -m "$dll_machine" -k -d "$dir/kernel32.def" -l "$dir/kernel32.lib"
    clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector -fno-optimize-sibling-calls \
        -Wall -Wextra -Werror -c arch/x86/test/select_stack_pe.c -o "$dir/probe.obj"
    lld-link /entry:select_stack_entry /subsystem:console /nodefaultlib /machine:"$machine" /fixed \
        "/out:$dir/selectstack$bits.exe" "$dir/probe.obj" "$dir/kernel32.lib"
done
