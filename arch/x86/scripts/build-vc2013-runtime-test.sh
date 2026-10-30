#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required}; mkdir -p "$out"
llvm-dlltool -m i386 -k -d arch/x86/test/wgl-backend-kernel32.def -l "$out/kernel32.lib"
clang --target=i686-pc-windows-msvc -O2 -ffreestanding -fno-builtin -fno-stack-protector -Wall -Wextra -Werror \
    -c arch/x86/test/vc2013_runtime_pe.c -o "$out/vcrun.obj"
lld-link /entry:vc2013_runtime_entry /subsystem:console /nodefaultlib /machine:x86 \
    /fixed "/out:$out/vcrun32.exe" "$out/vcrun.obj" "$out/kernel32.lib"
