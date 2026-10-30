#!/usr/bin/env bash
# Native binaries do not change Wine settings. csmt0/1 variants are for an
# isolated guest; they restore the previous value on normal completion.
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required}
for bits in 32 64; do
    dir="$out/pe$bits"; mkdir -p "$dir"
    if [[ $bits == 32 ]]; then
        target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
        cp arch/x86/test/wgl-backend-kernel32.def "$dir/kernel32.def"
    else
        target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64
        sed 's/@[0-9][0-9]*$//' arch/x86/test/wgl-backend-kernel32.def > "$dir/kernel32.def"
    fi
    llvm-dlltool -m "$dll_machine" -k -d "$dir/kernel32.def" -l "$dir/kernel32.lib"
    for mode in native csmt0 csmt1; do
        extra=()
        if [[ $mode != native ]]; then extra+=(-DD3D9_CSMT_SETTING="${mode#csmt}"); fi
        clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
            -Wall -Wextra -Werror -DD3D9_SYSTEM_BRIDGE -DD3D9_BENCHMARK "${extra[@]}" \
            -c arch/x86/test/d3d9_provider_pe.c -o "$dir/$mode.obj"
        lld-link /entry:d3d9_provider_entry /subsystem:console /nodefaultlib /machine:"$machine" \
            /fixed "/out:$dir/d3d9bench$bits-$mode.exe" "$dir/$mode.obj" "$dir/kernel32.lib"
    done
done
