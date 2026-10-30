#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required}
sdk=${2:?local SDK Include/version directory required}
msvc=${3:?local MSVC include directory required}
wdk=${4:?local directory containing d3dumddi.h required}
driver32=${5:?compiled PE32 driver directory required; build compiler.mk PLATFORM=win32 umd}
driver64=${6:?compiled PE64 driver directory required; build compiler.mk PLATFORM=win64 umd}
for bits in 32 64; do
    dir="$out/pe$bits"; mkdir -p "$dir"
    target=i686-pc-windows-msvc; machine=x86
    if [[ $bits == 64 ]]; then target=x86_64-pc-windows-msvc; machine=x64; fi
    source="$driver32"; [[ $bits == 64 ]] && source="$driver64"
    cp "$source/osito-intel9.dll" "$dir/osito-intel9.dll"
    cat > "$dir/kernel32.def" <<'EOF'
LIBRARY KERNEL32.dll
EXPORTS
LoadLibraryA@4
GetProcAddress@8
FreeLibrary@4
GetStdHandle@4
WriteFile@20
ExitProcess@4
SetErrorMode@4
GetModuleFileNameA@12
Sleep@4
EOF
    printf 'LIBRARY MSVCRT.dll\nEXPORTS\nmemset\nmemcpy\n' > "$dir/msvcrt.def"
    dll_machine=i386
    if [[ $bits == 64 ]]; then dll_machine=i386:x86-64; sed -i 's/@[0-9][0-9]*$//' "$dir/kernel32.def"; fi
    llvm-dlltool -m "$dll_machine" -k -d "$dir/kernel32.def" -l "$dir/kernel32.lib"
    llvm-dlltool -m "$dll_machine" -d "$dir/msvcrt.def" -l "$dir/msvcrt.lib"
    for mode in model rcs-model physical; do
        extra=(); [[ $mode == model ]] && extra=(-DUMD_MODEL)
        [[ $mode == rcs-model ]] && extra=(-DUMD_MODEL -DUMD_RCS_MODEL)
        clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
            -Wall -Wextra -Werror -Wno-unused-variable -Wno-ignored-attributes -Wno-pragma-pack -Wno-nonportable-include-path \
            -isystem "$msvc" -isystem "$sdk/um" -isystem "$sdk/shared" -isystem "$sdk/ucrt" -isystem "$wdk" \
            "${extra[@]}" -c arch/x86/test/native_umd_pe.c -o "$dir/probe-$mode.obj"
        lld-link /entry:native_umd_entry /subsystem:console /nodefaultlib /machine:"$machine" /timestamp:0 /fixed \
            "/out:$dir/umd$bits-$mode.exe" "$dir/probe-$mode.obj" "$dir/kernel32.lib" "$dir/msvcrt.lib"
    done
done
