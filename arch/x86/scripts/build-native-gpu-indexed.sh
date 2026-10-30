#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required}
for bits in 32 64; do
    dir="$out/pe$bits"; mkdir -p "$dir"
    cat > "$dir/kernel32.def" <<'EOF'
LIBRARY KERNEL32.dll
EXPORTS
LoadLibraryA@4
GetProcAddress@8
GetStdHandle@4
WriteFile@20
ExitProcess@4
Sleep@4
GetCurrentProcessId@0
GetModuleFileNameA@12
CreateProcessA@40
WaitForSingleObject@8
GetExitCodeProcess@8
CloseHandle@4
EOF
    if [[ $bits == 32 ]]; then
        target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
    else
        target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64
        sed -i 's/@[0-9][0-9]*$//' "$dir/kernel32.def"
    fi
    llvm-dlltool -m "$dll_machine" -k -d "$dir/kernel32.def" -l "$dir/kernel32.lib"
    for mode in draw; do
        clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
            -Wall -Wextra -Werror -I"$out/fixtures" -c arch/x86/test/native_gpu_indexed_pe.c -o "$dir/$mode.obj"
        lld-link /entry:native_gpu_indexed_entry /subsystem:console /nodefaultlib \
            /machine:"$machine" /fixed /timestamp:0 "/out:$dir/gpuindexed$bits.exe" \
            "$dir/$mode.obj" "$dir/kernel32.lib"
    done
done
