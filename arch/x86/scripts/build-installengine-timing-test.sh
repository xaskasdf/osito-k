#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required}
for bits in 32 64;do
    dir="$out/pe$bits";mkdir -p "$dir"
    target=i686-pc-windows-msvc;machine=x86;dll_machine=i386
    cp arch/x86/test/wgl-backend-kernel32.def "$dir/kernel32.def"
    if [[ $bits == 64 ]];then
        target=x86_64-pc-windows-msvc;machine=x64;dll_machine=i386:x86-64
        sed -i 's/@[0-9][0-9]*$//' "$dir/kernel32.def"
    fi
    llvm-dlltool -m "$dll_machine" -k -d "$dir/kernel32.def" -l "$dir/kernel32.lib"
    clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector -Wall -Wextra -Werror \
        -c arch/x86/test/installengine_timing_pe.c -o "$dir/probe.obj"
    lld-link /entry:timing_entry /subsystem:console /nodefaultlib /machine:"$machine" /fixed \
        "/out:$dir/instime$bits.exe" "$dir/probe.obj" "$dir/kernel32.lib"
    clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector -Wall -Wextra -Werror \
        -c arch/x86/test/installengine_timing_download_pe.c -o "$dir/download.obj"
    lld-link /entry:installengine_timing_download_entry /subsystem:console /nodefaultlib /machine:"$machine" /fixed \
        "/out:$dir/instimedl$bits.exe" "$dir/download.obj" "$dir/kernel32.lib"
done
