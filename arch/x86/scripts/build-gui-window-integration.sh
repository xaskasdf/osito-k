#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:-build/gui-window-integration}
mkdir -p "$out"
for bits in 32 64; do
    dir="$out/pe$bits"
    mkdir -p "$dir"
    if [[ $bits == 32 ]]; then
        target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
    else
        target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64
    fi
    cat > "$dir/kernel32.def" <<'DEFINITION'
LIBRARY kernel32.dll
EXPORTS
    ExitProcess@4
    GetStdHandle@4
    WriteFile@20
    GetModuleHandleA@4
    GetProcAddress@8
    LoadLibraryA@4
DEFINITION
    if [[ $bits == 64 ]]; then sed -i 's/@[0-9][0-9]*$//' "$dir/kernel32.def"; fi
    llvm-dlltool -m "$dll_machine" -k -d "$dir/kernel32.def" -l "$dir/kernel32.lib"
    clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
        -Wall -Wextra -Werror -c arch/x86/test/gui_window_integration_pe.c -o "$dir/gui-wm.obj"
    lld-link /machine:"$machine" /subsystem:console /entry:mainCRTStartup /nodefaultlib \
        /fixed /nxcompat /opt:ref /out:"$dir/gui-wm.exe" "$dir/gui-wm.obj" "$dir/kernel32.lib"
    printf '%s\n' "$dir/gui-wm.exe"
done
