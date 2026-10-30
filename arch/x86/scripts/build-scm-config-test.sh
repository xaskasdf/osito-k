#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required}
sdk=${2:?SDK include directory required}
msvc=${3:?MSVC include directory required}
for bits in 32 64; do
    dir="$out/pe$bits"; mkdir -p "$dir"
    target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
    if [[ $bits == 64 ]]; then target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64; fi
    cp arch/x86/test/wgl-backend-kernel32.def "$dir/kernel32.def"
    [[ $bits == 64 ]] && sed -i 's/@[0-9][0-9]*$//' "$dir/kernel32.def"
    llvm-dlltool -m "$dll_machine" -k -d "$dir/kernel32.def" -l "$dir/kernel32.lib"
    for phase in 0 1 2 3; do
        clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
            -Wall -Wextra -Werror -fms-extensions -Wno-ignored-attributes \
            -Wno-pragma-pack -Wno-nonportable-include-path -isystem "$msvc" \
            -isystem "$sdk/um" -isystem "$sdk/shared" -isystem "$sdk/ucrt" \
            -DSCM_PHASE="$phase" -c arch/x86/test/scm_config_pe.c -o "$dir/$phase.obj"
        lld-link /entry:scm_config_entry /subsystem:console /nodefaultlib /machine:"$machine" /fixed /timestamp:0 \
            "/out:$dir/scm$bits-$phase.exe" "$dir/$phase.obj" "$dir/kernel32.lib"
    done
done
