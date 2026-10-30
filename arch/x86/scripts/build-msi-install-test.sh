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
    cat > "$dir/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
ExitProcess@4
GetStdHandle@4
WriteFile@20
GetCommandLineW@0
GetModuleFileNameW@12
GetFileAttributesW@4
GetTickCount@0
Sleep@4
CreateThread@24
WaitForSingleObject@8
GetExitCodeThread@8
GetExitCodeProcess@8
CloseHandle@4
TerminateThread@8
TerminateProcess@8
CreateProcessW@40
EOF
    printf 'LIBRARY msi.dll\nEXPORTS\nMsiInstallProductW@8 @88 NONAME\nMsiGetProductInfoW@16 @70 NONAME\n' > "$dir/msi.def"
    for dll in kernel32 msi; do
        if [[ $bits == 64 ]]; then
            sed -i -E 's/@[0-9]+( |$)/\1/;s/ @[0-9]+ NONAME//' "$dir/$dll.def"
        fi
        llvm-dlltool -m "$dll_machine" -k -d "$dir/$dll.def" -l "$dir/$dll.lib"
    done
    clang --target="$target" -std=c11 -O2 -ffreestanding -fno-builtin -fno-stack-protector \
        -Wall -Wextra -Werror -c arch/x86/test/msi_install_pe.c -o "$dir/probe.obj"
    lld-link /entry:msi_install_entry /subsystem:console /nodefaultlib /machine:"$machine" \
        /fixed "/out:$dir/msiinstall$bits.exe" "$dir/probe.obj" "$dir/kernel32.lib" "$dir/msi.lib"
done
