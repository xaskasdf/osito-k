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
    printf 'LIBRARY kernel32.dll\nEXPORTS\nExitProcess@4\nGetStdHandle@4\nWriteFile@20\nGetCommandLineW@0\nGetModuleFileNameW@12\nGetCurrentDirectoryW@8\nSetCurrentDirectoryW@4\nSetEnvironmentVariableW@8\nCreateProcessW@40\nWaitForSingleObject@8\nGetExitCodeProcess@8\nCloseHandle@4\nTerminateProcess@8\n' > "$dir/kernel32.def"
    if [[ $bits == 64 ]]; then sed -i 's/@[0-9][0-9]*$//' "$dir/kernel32.def"; fi
    llvm-dlltool -m "$dll_machine" -k -d "$dir/kernel32.def" -l "$dir/kernel32.lib"
    clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
        -Wall -Wextra -Werror -c arch/x86/test/child_command_wide_pe.c -o "$dir/probe.obj"
    lld-link /entry:child_command_entry /subsystem:console /nodefaultlib /machine:"$machine" \
        /fixed "/out:$dir/cmdw$bits.exe" "$dir/probe.obj" "$dir/kernel32.lib"
done
