#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$repo"
out=${1:-arch/x86/build/memory-walk-test}
test ! -e "$out"
mkdir -p "$out"
cp arch/x86/test/memory_walk_pe.c "$out/"
for bits in 32 64; do
    dir="$out/pe$bits"
    mkdir -p "$dir"
    if [[ $bits == 32 ]]; then
        target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
        cp arch/x86/test/va-hint-kernel32.def "$dir/kernel32.def"
        printf 'VirtualProtect@16\n' >> "$dir/kernel32.def"
    else
        target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64
        sed 's/@[0-9][0-9]*$//' arch/x86/test/va-hint-kernel32.def > "$dir/kernel32.def"
        printf 'VirtualProtect\n' >> "$dir/kernel32.def"
    fi
    llvm-dlltool -m "$dll_machine" -k -d "$dir/kernel32.def" -l "$dir/kernel32.lib"
    clang --target="$target" -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin \
        -fno-stack-protector -fno-ident -c arch/x86/test/memory_walk_pe.c -o "$dir/walk.obj"
    lld-link /entry:memory_walk_entry /subsystem:console /nodefaultlib /machine:"$machine" /fixed \
        /nxcompat /timestamp:0 "/out:$dir/memorywalk$bits.exe" "$dir/walk.obj" "$dir/kernel32.lib"
done
