#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$repo"
out=${1:?output directory required}
mkdir -p "$out"
sed 's/@[0-9][0-9]*$//' arch/x86/test/suspended-image-kernel32.def > "$out/kernel32.def"
llvm-dlltool -m i386:x86-64 -d "$out/kernel32.def" -l "$out/kernel32.lib"
clang --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin -fno-stack-protector \
    -c arch/x86/test/growable_unwind_pe64.c -o "$out/growunwind.obj"
lld-link /entry:mainCRTStartup /subsystem:console /nodefaultlib /machine:x64 /fixed /timestamp:0 \
    "/out:$out/growunwind.exe" "$out/growunwind.obj" "$out/kernel32.lib"
