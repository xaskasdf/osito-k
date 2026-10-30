#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory}
sdk=${2:?SDK Include/version directory}
msvc=${3:?MSVC include directory}
wdk=${4:?WDK include directory}
fixtures=${5:?directory produced by assemble-native-uniform-tests.py}
# Run build-native-umd.sh first to supply our driver and import libraries.
for bits in 32 64; do
    dir="$out/pe$bits"; target=i686-pc-windows-msvc; machine=x86
    if [[ $bits == 64 ]]; then target=x86_64-pc-windows-msvc; machine=x64; fi
    for mode in model physical; do
        extra=(); [[ $mode == model ]] && extra=(-DUMD_MODEL -DUMD_RCS_MODEL)
        clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
            -Wall -Wextra -Werror -Wno-unused-variable -Wno-ignored-attributes -Wno-pragma-pack -Wno-nonportable-include-path \
            -isystem "$msvc" -isystem "$sdk/um" -isystem "$sdk/shared" -isystem "$sdk/ucrt" -isystem "$wdk" \
            "${extra[@]}" -I"$fixtures" -c arch/x86/test/native_pixel_constants_pe.c -o "$dir/uniform-$mode.obj"
        lld-link /entry:native_pixel_constants_entry /subsystem:console /nodefaultlib /machine:"$machine" /timestamp:0 /fixed \
            "/out:$dir/uniform$bits-$mode.exe" "$dir/uniform-$mode.obj" "$dir/kernel32.lib" "$dir/msvcrt.lib"
    done
done
