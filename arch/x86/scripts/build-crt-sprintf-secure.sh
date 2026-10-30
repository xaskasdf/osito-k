#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required}
for bits in 32 64; do
    dir="$out/pe$bits"; mkdir -p "$dir"
    target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
    if [[ $bits == 64 ]]; then target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64; fi
    cp arch/x86/test/wgl-backend-kernel32.def "$dir/kernel32.def"
    [[ $bits == 64 ]] && sed -i 's/@[0-9][0-9]*$//' "$dir/kernel32.def"
    llvm-dlltool -m "$dll_machine" -k -d "$dir/kernel32.def" -l "$dir/kernel32.lib"
    clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
        -Wall -Wextra -Werror -c arch/x86/test/crt_sprintf_secure_pe.c -o "$dir/probe.obj"
    lld-link /entry:sprintf_secure_entry /subsystem:console /nodefaultlib /machine:"$machine" /fixed /timestamp:0 \
        "/out:$dir/sprintfsec$bits.exe" "$dir/probe.obj" "$dir/kernel32.lib"
done
