#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required}
headers=${MINGW_HEADERS:-/usr/share/mingw-w64/include}
if [[ ! -d $headers ]]; then headers=/mnt/c/msys64/mingw64/include;fi
for bits in 32 64; do
    dir="$out/pe$bits";mkdir -p "$dir"
    if [[ $bits == 32 ]]; then target=i686-w64-windows-gnu;machine=x86;dll_machine=i386
    else target=x86_64-w64-windows-gnu;machine=x64;dll_machine=i386:x86-64;fi
    printf 'LIBRARY kernel32.dll\nEXPORTS\nExitProcess@4\nGetStdHandle@4\nWriteFile@20\nLoadLibraryA@4\nGetProcAddress@8\nGetCommandLineA@0\nVirtualAlloc@16\nVirtualProtect@16\nVirtualFree@12\n' > "$dir/kernel32.def"
    if [[ $bits == 64 ]]; then sed -i 's/@[0-9][0-9]*$//' "$dir/kernel32.def";fi
    llvm-dlltool -m "$dll_machine" -k -d "$dir/kernel32.def" -l "$dir/kernel32.lib"
    clang --target="$target" -isystem "$headers" -std=c11 -O2 -ffreestanding -fno-builtin -fno-stack-protector \
        -mno-stack-arg-probe -Wall -Wextra -Werror -c arch/x86/test/ipinterface_pe.c -o "$dir/probe.obj"
    lld-link /entry:ipinterface_entry /subsystem:console /nodefaultlib /machine:"$machine" \
        /fixed "/out:$dir/ipinterface$bits.exe" "$dir/probe.obj" "$dir/kernel32.lib"
done
