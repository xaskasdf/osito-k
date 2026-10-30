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
    printf 'LIBRARY kernel32.dll\nEXPORTS\nExitProcess@4\nGetStdHandle@4\nWriteFile@20\nLoadLibraryA@4\nLoadLibraryW@4\nLoadLibraryExA@12\nLoadLibraryExW@12\nGetProcAddress@8\nFreeLibrary@4\nGetLastError@0\nSetLastError@4\n' > "$dir/kernel32.def"
    if [[ $bits == 64 ]]; then sed -i 's/@[0-9][0-9]*$//' "$dir/kernel32.def"; fi
    llvm-dlltool -m "$dll_machine" -k -d "$dir/kernel32.def" -l "$dir/kernel32.lib"
    clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
        -Wall -Wextra -Werror -c arch/x86/test/module_name_pe.c -o "$dir/probe.obj"
    lld-link /entry:module_name_entry /subsystem:console /nodefaultlib /machine:"$machine" \
        /fixed "/out:$dir/modname$bits.exe" "$dir/probe.obj" "$dir/kernel32.lib"
    clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
        -Wall -Wextra -Werror -c arch/x86/test/system_path_probe_dll.c -o "$dir/dll.obj"
    lld-link /dll /noentry /nodefaultlib /machine:"$machine" \
        "/out:$dir/xinput-probe.dll" "$dir/dll.obj"
    for name in XINPUT1_3.dll dinput-probe.dll hidapi.dll; do
        cp "$dir/xinput-probe.dll" "$dir/$name"
    done
done
