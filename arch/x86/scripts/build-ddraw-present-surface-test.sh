#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required}
mkdir -p "$out"
llvm-dlltool -m i386 -k -d arch/x86/test/wgl-backend-kernel32.def -l "$out/kernel32.lib"
clang --target=i686-pc-windows-msvc -O2 -ffreestanding -fno-builtin \
    -fno-stack-protector -Wall -Wextra -Werror \
    -c arch/x86/test/ddraw_present_surface_pe32.c -o "$out/ddraw-surface32.obj"
lld-link /entry:mainCRTStartup /subsystem:console /nodefaultlib /machine:x86 /fixed \
    "/out:$out/ddraw-surface32.exe" "$out/ddraw-surface32.obj" "$out/kernel32.lib"
