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
FreeLibrary@4
GetStdHandle@4
WriteFile@20
ExitProcess@4
SetErrorMode@4
EOF
    target=i686-pc-windows-msvc;machine=x86;dll_machine=i386
    if [[ $bits == 64 ]]; then
        target=x86_64-pc-windows-msvc;machine=x64;dll_machine=i386:x86-64
        sed -i 's/@[0-9][0-9]*$//' "$dir/kernel32.def"
    fi
    llvm-dlltool -m "$dll_machine" -k -d "$dir/kernel32.def" -l "$dir/kernel32.lib"
    for mode in empty native; do
        flags=(); [[ $mode == empty ]] && flags+=(-DKMT_SCANLINE_EXPECT_EMPTY)
        clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector "${flags[@]}" \
            -Wall -Wextra -Werror -c arch/x86/test/native_scanline_pe.c -o "$dir/scanline.obj"
        lld-link /entry:native_scanline_entry /subsystem:console /nodefaultlib \
            /machine:"$machine" /fixed /timestamp:0 "/out:$dir/scanline$bits-$mode.exe" \
            "$dir/scanline.obj" "$dir/kernel32.lib"
    done
done
