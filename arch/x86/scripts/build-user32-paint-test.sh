#!/bin/sh
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
x86_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
out_dir=${1:-"$x86_dir/build/test-user32-paint"}
for arch in 32 64; do
    dir="$out_dir/pe$arch"
    mkdir -p "$dir"
    if [ "$arch" = 32 ]; then
        target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
    else
        target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64
    fi
    for dll in kernel32 user32 gdi32; do
        source="$x86_dir/test/gdi-paint-$dll.def"
        [ "$dll" != user32 ] || source="$x86_dir/test/user32-paint-user32.def"
        if [ "$arch" = 64 ]; then
            sed 's/@[0-9][0-9]*$//' "$source" > "$dir/$dll.def"
        else
            cp "$source" "$dir/$dll.def"
        fi
        "${LLVM_DLLTOOL:-llvm-dlltool}" -m "$dll_machine" -k \
            -d "$dir/$dll.def" -l "$dir/$dll.lib"
    done
    "${CLANG:-clang}" --target="$target" -O2 -ffreestanding \
        -fno-builtin -fno-stack-protector -fno-ident -c \
        "$x86_dir/test/user32_paint_pe.c" -o "$dir/user32_paint.obj"
    "${LLD_LINK:-lld-link}" /machine:"$machine" /subsystem:console \
        /entry:mainCRTStartup /nodefaultlib /fixed /nxcompat /opt:ref \
        /out:"$dir/user32_paint_pe$arch.exe" "$dir/user32_paint.obj" \
        "$dir/kernel32.lib" "$dir/user32.lib" "$dir/gdi32.lib"
    printf '%s\n' "$dir/user32_paint_pe$arch.exe"
done
