#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory};sdk=${2:?SDK include};msvc=${3:?MSVC include}
for bits in 32 64; do
 dir="$out/pe$bits";mkdir -p "$dir"
 target=i686-pc-windows-msvc;machine=x86;dll_machine=i386
 [[ $bits == 64 ]] && { target=x86_64-pc-windows-msvc;machine=x64;dll_machine=i386:x86-64; }
 cat > "$dir/kernel32.def" <<'EOF'
LIBRARY KERNEL32.dll
EXPORTS
LoadLibraryA@4
GetProcAddress@8
GetStdHandle@4
WriteFile@20
ExitProcess@4
SetErrorMode@4
VirtualAlloc@16
VirtualFree@12
EOF
 [[ $bits == 64 ]] && sed -i 's/@[0-9][0-9]*$//' "$dir/kernel32.def"
 llvm-dlltool -m "$dll_machine" -k -d "$dir/kernel32.def" -l "$dir/kernel32.lib"
 clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector -fms-extensions \
  -Wall -Wextra -Werror -Wno-ignored-attributes -Wno-pragma-pack -Wno-nonportable-include-path \
  -DWIN32_LEAN_AND_MEAN -isystem "$msvc" -isystem "$sdk/um" -isystem "$sdk/shared" -isystem "$sdk/ucrt" \
  -c arch/x86/test/native_residency_windows.c -o "$dir/probe.obj"
 lld-link /entry:residency_windows_entry /subsystem:console /nodefaultlib /machine:"$machine" /fixed /timestamp:0 \
  "/out:$dir/residency$bits-windows.exe" "$dir/probe.obj" "$dir/kernel32.lib"
done
