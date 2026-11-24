#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required}
for bits in 32 64; do
    dir="$out/pe$bits"; mkdir -p "$dir"
    cp arch/x86/test/wgl-backend-kernel32.def "$dir/kernel32.def"
    printf 'GetModuleHandleA@4\nGetModuleHandleExA@12\nGetModuleFileNameA@12\nFreeLibrary@4\nSetErrorMode@4\n' >> "$dir/kernel32.def"
    if [[ $bits == 32 ]]; then
        target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
    else
        target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64
        sed -i 's/@[0-9][0-9]*$//' "$dir/kernel32.def"
    fi
    llvm-dlltool -m "$dll_machine" -k -d "$dir/kernel32.def" -l "$dir/kernel32.lib"
    for mode in fixture absent runtime incomplete; do
        extra=()
        [[ $mode == absent ]] && extra=(-DDXTEST_ABSENT)
        [[ $mode == runtime ]] && extra=(-DDXTEST_RUNTIME)
        [[ $mode == incomplete ]] && extra=(-DDXTEST_INCOMPLETE)
        clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
            -Wall -Wextra -Werror "${extra[@]}" -c arch/x86/test/native_d3d9_pe.c -o "$dir/$mode.obj"
        lld-link /entry:native_d3d9_entry /subsystem:console /nodefaultlib /machine:"$machine" \
            /fixed "/out:$dir/dxloader$bits-$mode.exe" "$dir/$mode.obj" "$dir/kernel32.lib"
    done
    mkdir -p "$dir/fixtures"
    for library in d3d9 dxgi; do
        tag=0x44580009U; base=0x60000000
        if [[ $library == dxgi ]]; then tag=0x4458000AU; base=0x61000000; fi
        clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
            -Wall -Wextra -Werror -DDXTEST_DLL="$tag" -c arch/x86/test/native_d3d9_pe.c -o "$dir/$library.obj"
        lld-link /dll /noentry /nodefaultlib /machine:"$machine" /base:"$base" \
            /export:dx_loader_identity "/out:$dir/fixtures/$library.dll" "$dir/$library.obj"
        clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
            -Wall -Wextra -Werror -DDXTEST_STATIC="$tag" -c arch/x86/test/native_d3d9_pe.c -o "$dir/static-$library.obj"
        lld-link /entry:native_d3d9_entry /subsystem:console /nodefaultlib /machine:"$machine" \
            /fixed "/out:$dir/dxloader$bits-static-$library.exe" "$dir/static-$library.obj" \
            "$dir/kernel32.lib" "$dir/fixtures/$library.lib"
    done
    mkdir -p "$dir/incomplete"
    printf 'LIBRARY GDI32.dll\nEXPORTS\nOsitoMissingDriverContract@0\n' > "$dir/missing.def"
    if [[ $bits == 64 ]]; then sed -i 's/@[0-9][0-9]*$//' "$dir/missing.def"; fi
    llvm-dlltool -m "$dll_machine" -k -d "$dir/missing.def" -l "$dir/missing.lib"
    clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
        -Wall -Wextra -Werror -DDXTEST_DLL=1 -DDXTEST_MISSING_IMPORT \
        -c arch/x86/test/native_d3d9_pe.c -o "$dir/incomplete.obj"
    for library in d3d9 dxgi; do
        lld-link /dll /noentry /nodefaultlib /machine:"$machine" /base:0x62000000 \
            /export:dx_loader_identity "/out:$dir/incomplete/$library.dll" "$dir/incomplete.obj" "$dir/missing.lib"
    done
done
