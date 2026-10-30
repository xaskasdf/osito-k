#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$repo"
out=${1:-arch/x86/build/child-capacity-test}
test ! -e "$out"
mkdir -p "$out"
cp arch/x86/test/child_capacity_pe.c arch/x86/test/child-capacity-kernel32.def "$out/"
for bits in 32 64; do
    if [[ $bits == 32 ]]; then
        target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
        cp arch/x86/test/child-capacity-kernel32.def "$out/kernel32.def"
    else
        target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64
        sed 's/@[0-9][0-9]*$//' arch/x86/test/child-capacity-kernel32.def > "$out/kernel32.def"
    fi
    llvm-dlltool -m "$dll_machine" -k -d "$out/kernel32.def" -l "$out/kernel32.lib"
    clang --target="$target" -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin \
        -fno-stack-protector -c arch/x86/test/child_capacity_pe.c -o "$out/capacity$bits.obj"
    lld-link /entry:mainCRTStartup /subsystem:console /nodefaultlib /machine:"$machine" /fixed \
        "/out:$out/capacity$bits.exe" "$out/capacity$bits.obj" "$out/kernel32.lib"
done
