#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required}
for bits in 32 64; do
    dir="$out/pe$bits";mkdir -p "$dir"
    if [[ $bits == 32 ]]; then target=i686-pc-windows-msvc;machine=x86;dll_machine=i386
    else target=x86_64-pc-windows-msvc;machine=x64;dll_machine=i386:x86-64;fi
    printf 'LIBRARY kernel32.dll\nEXPORTS\nExitProcess@4\nGetStdHandle@4\nWriteFile@20\nReadFile@20\nLoadLibraryA@4\nGetProcAddress@8\nCreateFileA@28\nCloseHandle@4\nDeleteFileA@4\nGetFileSize@8\nSetFilePointer@16\nGetLastError@0\nSetLastError@4\n' > "$dir/kernel32.def"
    if [[ $bits == 64 ]]; then sed -i 's/@[0-9][0-9]*$//' "$dir/kernel32.def";fi
    llvm-dlltool -m "$dll_machine" -k -d "$dir/kernel32.def" -l "$dir/kernel32.lib"
    clang --target="$target" -std=c11 -O2 -ffreestanding -fno-builtin -fno-stack-protector -mno-stack-arg-probe \
        -Wall -Wextra -Werror -c arch/x86/test/imagehlp_pe.c -o "$dir/probe.obj"
    lld-link /entry:imagehlp_entry /subsystem:console /nodefaultlib /machine:"$machine" /fixed \
        "/out:$dir/imagehlp$bits.exe" "$dir/probe.obj" "$dir/kernel32.lib"
done
