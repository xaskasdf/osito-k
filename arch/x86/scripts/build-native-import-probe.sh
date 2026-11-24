#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory}; sdk=${2:?local SDK include/version}; msvc=${3:?local MSVC include}
for bits in 32 64; do
    dir="$out/pe$bits"; mkdir -p "$dir"
    target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
    if [[ $bits == 64 ]]; then target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64; fi
    cat > "$dir/kernel32.def" <<'EOF'
LIBRARY KERNEL32.dll
EXPORTS
LoadLibraryA@4
GetCommandLineA@0
GetLastError@0
FreeLibrary@4
GetStdHandle@4
WriteFile@20
ExitProcess@4
EOF
    [[ $bits == 64 ]] && sed -i 's/@[0-9][0-9]*$//' "$dir/kernel32.def"
    llvm-dlltool -m "$dll_machine" -k -d "$dir/kernel32.def" -l "$dir/kernel32.lib"
    clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
        -Wall -Wextra -Werror -Wno-ignored-attributes -Wno-pragma-pack -Wno-nonportable-include-path \
        -isystem "$msvc" -isystem "$sdk/um" -isystem "$sdk/shared" -isystem "$sdk/ucrt" \
        -c arch/x86/test/native_import_probe_pe.c -o "$dir/probe.obj"
    lld-link /entry:native_import_probe_entry /subsystem:console /nodefaultlib /machine:"$machine" /timestamp:0 /fixed \
        "/out:$dir/import$bits.exe" "$dir/probe.obj" "$dir/kernel32.lib"
done
