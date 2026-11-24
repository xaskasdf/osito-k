#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-build/native-shader-host}
fixtures=${2:-build/native-shader-fixtures}
mkdir -p "$out"
ulimit -c 0
for cc in gcc clang; do
    for mode in normal sanitize; do
        flags=()
        if [[ $mode == sanitize ]]; then
            flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        fi
        "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror "${flags[@]}" \
            test/intel_d3d9_shader_host.c lib/intel_d3d9/shader.c -o "$out/shader-$cc-$mode"
        timeout 60 "$out/shader-$cc-$mode" "$fixtures" | tee "$out/$cc-$mode.log"
    done
done
# Compile the exact decoder for both native driver ABIs without SDK headers.
for bits in 32 64; do
    target=i686-pc-windows-msvc
    machine=x86; dll_machine=i386
    [[ $bits == 64 ]] && { target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64; }
    clang --target="$target" -std=c11 -O2 -ffreestanding -fno-builtin -fno-stack-protector \
        -Wall -Wextra -Werror -c lib/intel_d3d9/shader.c -o "$out/shader$bits.obj"
    clang --target="$target" -std=c11 -O2 -ffreestanding -fno-builtin -fno-stack-protector \
        -Wall -Wextra -Werror -I"$fixtures" -c test/native_shader_pe.c -o "$out/probe$bits.obj"
    printf 'LIBRARY KERNEL32.dll\nEXPORTS\nGetStdHandle@4\nWriteFile@20\nExitProcess@4\n' > "$out/kernel32-$bits.def"
    [[ $bits == 64 ]] && sed -i 's/@[0-9][0-9]*$//' "$out/kernel32-$bits.def"
    llvm-dlltool -m "$dll_machine" -k -d "$out/kernel32-$bits.def" -l "$out/kernel32-$bits.lib"
    lld-link /entry:native_shader_entry /subsystem:console /nodefaultlib /machine:"$machine" /timestamp:0 /fixed \
        "/out:$out/shader$bits.exe" "$out/probe$bits.obj" "$out/shader$bits.obj" "$out/kernel32-$bits.lib"
done
