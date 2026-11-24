#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required};mkdir -p "$out"
printf 'LIBRARY kernel32.dll\nEXPORTS\nExitProcess@4\nGetStdHandle@4\nWriteFile@20\nLoadLibraryA@4\nGetProcAddress@8\nFreeLibrary@4\nGetLastError@0\n' > "$out/kernel32.def"
printf 'CreateProcessA@40\nOpenEventA@12\nCloseHandle@4\nGetExitCodeProcess@8\nSleep@4\n' >> "$out/kernel32.def"
llvm-dlltool -m i386 -k -d "$out/kernel32.def" -l "$out/kernel32.lib"
clang --target=i686-pc-windows-msvc -std=c11 -O2 -ffreestanding -fno-builtin -fno-stack-protector \
    -mno-stack-arg-probe -Wall -Wextra -Werror -c arch/x86/test/steam_interface_pe.c -o "$out/probe.obj"
lld-link /entry:steam_interface_entry /subsystem:console /nodefaultlib /machine:x86 /fixed \
    "/out:$out/steaminterface32.exe" "$out/probe.obj" "$out/kernel32.lib"
clang --target=i686-pc-windows-msvc -std=c11 -O2 -ffreestanding -fno-builtin -fno-stack-protector \
    -mno-stack-arg-probe -Wall -Wextra -Werror -DSTEAMIFACE_START_CLIENT \
    -c arch/x86/test/steam_interface_pe.c -o "$out/ipc-probe.obj"
lld-link /entry:steam_interface_entry /subsystem:console /nodefaultlib /machine:x86 /fixed \
    "/out:$out/steamipc32.exe" "$out/ipc-probe.obj" "$out/kernel32.lib"
