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
CreateThread@24
WaitForSingleObject@8
CloseHandle@4
GetCommandLineA@0
GetModuleFileNameA@12
CreateProcessA@40
GetExitCodeProcess@8
CreateEventA@16
OpenEventA@12
SetEvent@4
ResetEvent@4
TerminateProcess@8
Sleep@4
EOF
    cat > "$dir/msi.def" <<'EOF'
LIBRARY msi.dll
EXPORTS
MsiCloseHandle@4 @8 NONAME
MsiCreateRecord@4 @17 NONAME
MsiRecordGetFieldCount@4 @115 NONAME
MsiRecordGetInteger@8 @116 NONAME
MsiRecordGetStringW@16 @118 NONAME
MsiRecordSetStringW@12 @125 NONAME
MsiFormatRecordW@16 @171 NONAME
MsiRecordSetInteger@12
MsiRecordIsNull@8
MsiRecordDataSize@8
EOF
    for dll in kernel32 msi; do
        if [[ $bits == 64 ]]; then
            sed -i -E 's/@[0-9]+( |$)/\1/;s/ @[0-9]+ NONAME//' "$dir/$dll.def"
        fi
        llvm-dlltool -m "$dll_machine" -k -d "$dir/$dll.def" -l "$dir/$dll.lib"
    done
    clang --target="$target" -std=c11 -O2 -ffreestanding -fno-builtin -fno-stack-protector \
        -Wall -Wextra -Werror -c arch/x86/test/msi_records_pe.c -o "$dir/probe.obj"
    lld-link /entry:msi_records_entry /subsystem:console /nodefaultlib /machine:"$machine" \
        /fixed "/out:$dir/msirecord$bits.exe" "$dir/probe.obj" "$dir/kernel32.lib" "$dir/msi.lib"
done
