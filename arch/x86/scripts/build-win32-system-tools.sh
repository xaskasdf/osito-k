#!/usr/bin/env bash
# Build only Osito-K sources, never copy or link Microsoft runtime payloads.
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:-arch/x86/build/win32-system-tools}
for bits in 32 64; do
    dir="$out/obj$bits"; mkdir -p "$dir"
    if [[ $bits == 32 ]]; then
        target=i686-pc-windows-msvc; machine=x86; dll_machine=i386; system=SysWOW64
    else
        target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64; system=System32
    fi
    printf 'LIBRARY kernel32.dll\nEXPORTS\nExitProcess@4\nGetCommandLineW@0\nLocalAlloc@8\nLocalFree@4\nLoadLibraryExW@12\nGetProcAddress@8\nFreeLibrary@4\nGetLastError@0\nSetErrorMode@4\n' > "$dir/kernel32.def"
    printf 'LIBRARY shell32.dll\nEXPORTS\nCommandLineToArgvW@8\n' > "$dir/shell32.def"
    printf 'LIBRARY ole32.dll\nEXPORTS\nOleInitialize@4\nOleUninitialize@0\n' > "$dir/ole32.def"
    printf 'LIBRARY user32.dll\nEXPORTS\nMessageBoxW@16\n' > "$dir/user32.def"
    libraries=()
    for dll in kernel32 shell32 ole32 user32; do
        if [[ $bits == 64 ]]; then sed -i 's/@[0-9][0-9]*$//' "$dir/$dll.def"; fi
        llvm-dlltool -m "$dll_machine" -k -d "$dir/$dll.def" -l "$dir/$dll.lib"
        libraries+=("$dir/$dll.lib")
    done
    clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
        -Wall -Wextra -Werror -c arch/x86/win32/programs/regsvr32.c -o "$dir/regsvr32.obj"
    mkdir -p "$out/Windows/$system"
    lld-link /entry:regsvr_entry /subsystem:windows /nodefaultlib /machine:"$machine" \
        /timestamp:0 "/out:$out/Windows/$system/regsvr32.exe" "$dir/regsvr32.obj" "${libraries[@]}"
done
