#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required}
GPUCOPY_MODES='copy paging lock2-sync' bash arch/x86/scripts/build-native-gpu-copy.sh "$out"
for bits in 32 64; do
    dir="$out/pe$bits"
    target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
    [[ $bits == 64 ]] && { target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64; }
    printf 'LIBRARY KERNEL32.dll\nEXPORTS\nSetErrorMode@4\n' > "$dir/paging-kernel32.def"
    [[ $bits == 64 ]] && sed -i 's/@[0-9][0-9]*$//' "$dir/paging-kernel32.def"
    llvm-dlltool -m "$dll_machine" -k -d "$dir/paging-kernel32.def" -l "$dir/paging-kernel32.lib"
    for mode in empty oracle physical exit; do
    flags=()
    [[ $mode != empty ]] && flags+=(-DPAGING_PHYSICAL)
    [[ $mode == oracle ]] && flags+=(-DPAGING_ORACLE)
    [[ $mode == exit ]] && flags+=(-DPAGING_EXIT)
    clang "${flags[@]}" --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
        -Wall -Wextra -Werror -c arch/x86/test/native_paging_pe.c -o "$dir/paging-$mode.obj"
    lld-link /entry:native_paging_entry /subsystem:console /nodefaultlib /machine:"$machine" /fixed /timestamp:0 \
        "/out:$dir/paging$bits-$mode.exe" "$dir/paging-$mode.obj" "$dir/kernel32.lib" "$dir/paging-kernel32.lib"
    done
done
