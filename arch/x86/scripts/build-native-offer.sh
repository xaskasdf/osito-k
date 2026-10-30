#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory}
for bits in 32 64; do
    dir="$out/pe$bits";mkdir -p "$dir"
    target=i686-pc-windows-msvc;machine=x86;dll_machine=i386
    [[ $bits == 64 ]] && { target=x86_64-pc-windows-msvc;machine=x64;dll_machine=i386:x86-64; }
    printf '%s\n' 'LIBRARY KERNEL32.dll' 'EXPORTS' 'LoadLibraryA@4' 'GetProcAddress@8' 'GetStdHandle@4' 'WriteFile@20' 'ExitProcess@4' 'SetErrorMode@4' > "$dir/kernel32.def"
    [[ $bits == 64 ]] && sed -i 's/@[0-9][0-9]*$//' "$dir/kernel32.def"
    llvm-dlltool -m "$dll_machine" -k -d "$dir/kernel32.def" -l "$dir/kernel32.lib"
    for mode in negative physical exit; do
        flags=();[[ $mode != negative ]] && flags+=(-DOFFER_PHYSICAL)
        [[ $mode == exit ]] && flags+=(-DOFFER_EXIT)
        clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector -Wall -Wextra -Werror \
            "${flags[@]}" -c arch/x86/test/native_offer_pe.c -o "$dir/offer-$mode.obj"
        lld-link /entry:native_offer_entry /subsystem:console /nodefaultlib /machine:"$machine" /fixed /timestamp:0 \
            "/out:$dir/offer$bits-$mode.exe" "$dir/offer-$mode.obj" "$dir/kernel32.lib"
    done
done
