#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$repo"
out=${1:?output directory required}
mkdir -p "$out"
sed 's/@[0-9][0-9]*$//' arch/x86/test/suspended-image-kernel32.def > "$out/kernel32.def"
llvm-dlltool -m i386:x86-64 -d "$out/kernel32.def" -l "$out/kernel32.lib"
clang --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin -fno-stack-protector \
    -c arch/x86/test/nt64_service_pe.c -o "$out/nt64-service.obj"
clang --target=x86_64-pc-windows-msvc -c arch/x86/test/nt64_service_probe.S -o "$out/probe.obj"
lld-link /entry:mainCRTStartup /subsystem:console /nodefaultlib /machine:x64 /fixed \
    "/out:$out/nt64-service.exe" "$out/nt64-service.obj" "$out/probe.obj" "$out/kernel32.lib"
clang --target=x86_64-linux-gnu -O2 -ffreestanding -fno-stack-protector \
    -mgeneral-regs-only -mno-red-zone -fno-pie -nostdlib -static -Wl,-e,_start \
    arch/x86/test/nt64_service_linux.c -o "$out/nt64-elf"
