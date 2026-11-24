#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required}
GPUCOPY_MODES='copy lock2 lock2-sync lock2-exit' bash arch/x86/scripts/build-native-gpu-copy.sh "$out"
for bits in 32 64; do
    dir="$out/pe$bits"
    target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
    [[ $bits == 64 ]] && { target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64; }
    printf 'LIBRARY KERNEL32.dll\nEXPORTS\nSetErrorMode@4\n' > "$dir/lock2-kernel32.def"
    [[ $bits == 64 ]] && sed -i 's/@[0-9][0-9]*$//' "$dir/lock2-kernel32.def"
    llvm-dlltool -m "$dll_machine" -k -d "$dir/lock2-kernel32.def" -l "$dir/lock2-kernel32.lib"
    clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
        -Wall -Wextra -Werror -c arch/x86/test/native_lock2_pe.c -o "$dir/lock2.obj"
    lld-link /entry:native_lock2_entry /subsystem:console /nodefaultlib /machine:"$machine" /fixed /timestamp:0 \
        "/out:$dir/lock2$bits.exe" "$dir/lock2.obj" "$dir/kernel32.lib" "$dir/lock2-kernel32.lib"
done
