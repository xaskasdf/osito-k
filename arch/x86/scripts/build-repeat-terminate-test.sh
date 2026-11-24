#!/bin/sh
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
x86_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
out_dir=${1:-"$x86_dir/build/test-repeat-terminate"}
for bits in 32 64; do
    dir="$out_dir/pe$bits"
    mkdir -p "$dir"
    if [ "$bits" = 32 ]; then
        target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
        cp "$x86_dir/test/child-exit-kernel32.def" "$dir/kernel32.def"
    else
        target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64
        sed 's/@[0-9][0-9]*$//' "$x86_dir/test/child-exit-kernel32.def" > "$dir/kernel32.def"
    fi
    "${LLVM_DLLTOOL:-llvm-dlltool}" -m "$dll_machine" -k \
        -d "$dir/kernel32.def" -l "$dir/kernel32.lib"
    "${CLANG:-clang}" --target="$target" -O2 -ffreestanding \
        -fno-builtin -fno-stack-protector -fno-ident -c \
        "$x86_dir/test/repeat_terminate_pe.c" -o "$dir/repeat-terminate.obj"
    "${LLD_LINK:-lld-link}" /machine:"$machine" /subsystem:console \
        /entry:repeat_terminate_entry /nodefaultlib /fixed /nxcompat /opt:ref /timestamp:0 \
        /out:"$dir/repeatterminate$bits.exe" "$dir/repeat-terminate.obj" "$dir/kernel32.lib"
done
