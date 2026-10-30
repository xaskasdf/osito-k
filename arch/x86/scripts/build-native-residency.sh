#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory}
bash arch/x86/scripts/build-native-d3dkmt-test.sh "$out" "${2:-}" "${3:-}"
for bits in 32 64; do
 dir="$out/pe$bits";target=i686-pc-windows-msvc;machine=x86
 [[ $bits == 64 ]] && { target=x86_64-pc-windows-msvc;machine=x64; }
 clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector -Wall -Wextra -Werror \
  -c arch/x86/test/native_residency_pe.c -o "$dir/residency.obj"
 lld-link /entry:native_residency_entry /subsystem:console /nodefaultlib /machine:"$machine" /fixed /timestamp:0 \
  "/out:$dir/residency$bits.exe" "$dir/residency.obj" "$dir/kernel32.lib"
done
GPUCOPY_MODES='copy paging lock2-sync residency' bash arch/x86/scripts/build-native-gpu-copy.sh "$out"
