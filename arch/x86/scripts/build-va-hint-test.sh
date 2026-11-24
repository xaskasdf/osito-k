#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$repo"
out=${1:-arch/x86/build/va-hint-test}
test ! -e "$out"
mkdir -p "$out"
cp arch/x86/test/va_hint_pe.c arch/x86/test/va-hint-kernel32.def "$out/"
for bits in 32 64; do
    mkdir -p "$out/pe$bits"
    dir="$out/pe$bits"
    if [[ $bits == 32 ]]; then
        target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
        cp arch/x86/test/va-hint-kernel32.def "$dir/kernel32.def"
    else
        target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64
        sed 's/@[0-9][0-9]*$//' arch/x86/test/va-hint-kernel32.def > "$dir/kernel32.def"
    fi
    llvm-dlltool -m "$dll_machine" -k -d "$dir/kernel32.def" -l "$dir/kernel32.lib"
    clang --target="$target" -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin \
        -fno-stack-protector -fno-ident -c arch/x86/test/va_hint_pe.c -o "$dir/hint.obj"
    lld-link /entry:mainCRTStartup /subsystem:console /nodefaultlib /machine:"$machine" /fixed \
        /nxcompat /timestamp:0 "/out:$dir/vahint$bits.exe" "$dir/hint.obj" "$dir/kernel32.lib"
done
