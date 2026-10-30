#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required}
for bits in 32 64; do
    dir="$out/pe$bits"; mkdir -p "$dir"
    if [[ $bits == 32 ]]; then
        target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
    else
        target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64
    fi
    printf 'LIBRARY kernel32.dll\nEXPORTS\nExitProcess@4\nGetStdHandle@4\nWriteFile@20\nGetCommandLineA@0\nCreateThread@24\nWaitForSingleObject@8\nCloseHandle@4\n' > "$dir/kernel32.def"
    printf 'LIBRARY msi.dll\nEXPORTS\nMsiEnumProductsW@8 @45 NONAME\nMsiEnumRelatedProductsW@16 @205 NONAME\nMsiQueryFeatureStateW@8 @111 NONAME\n' > "$dir/msi.def"
    printf 'LIBRARY advapi32.dll\nEXPORTS\nRegCreateKeyExW@36\nRegSetValueExW@24\nRegDeleteKeyW@8\nRegDeleteKeyExW@16\nRegCloseKey@4\n' > "$dir/advapi32.def"
    for dll in kernel32 msi advapi32; do
        if [[ $bits == 64 ]]; then
            sed -i -E 's/@[0-9]+( |$)/\1/;s/ @[0-9]+ NONAME//' "$dir/$dll.def"
        fi
        llvm-dlltool -m "$dll_machine" -k -d "$dir/$dll.def" -l "$dir/$dll.lib"
    done
    clang --target="$target" -std=c11 -O2 -ffreestanding -fno-builtin -fno-stack-protector \
        -Wall -Wextra -Werror -c arch/x86/test/msi_discovery_pe.c -o "$dir/probe.obj"
    lld-link /entry:msi_discovery_entry /subsystem:console /nodefaultlib /machine:"$machine" \
        /fixed "/out:$dir/msidiscovery$bits.exe" "$dir/probe.obj" "$dir/kernel32.lib" "$dir/msi.lib" "$dir/advapi32.lib"
done
