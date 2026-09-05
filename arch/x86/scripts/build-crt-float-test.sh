#!/bin/sh
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
x86_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
out_dir=${1:-"$x86_dir/build/test-crt-float"}
for arch in 32 64; do
    dir="$out_dir/pe$arch"
    mkdir -p "$dir"
    if [ "$arch" = 32 ]; then
        target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
        cp "$x86_dir/test/crt-float-kernel32.def" "$dir/kernel32.def"
        local_flags=
        set --
    else
        target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64
        sed 's/@[0-9][0-9]*$//' "$x86_dir/test/crt-float-kernel32.def" > "$dir/kernel32.def"
        local_flags=-DTEST_LOCAL_FLOAT; local_object="$dir/converter.obj"
        "${CLANG:-clang}" --target="$target" -O2 -ffreestanding -fno-builtin \
            -fno-stack-protector -c "$x86_dir/win32/crt_float.c" -o "$local_object"
        set -- "$local_object"
    fi
    "${LLVM_DLLTOOL:-llvm-dlltool}" -m "$dll_machine" -k \
        -d "$dir/kernel32.def" -l "$dir/kernel32.lib"
    "${CLANG:-clang}" --target="$target" -O2 -msse2 -ffreestanding \
        -fno-builtin -fno-stack-protector -fno-ident $local_flags -c \
        "$x86_dir/test/crt_float_pe.c" -o "$dir/float.obj"
    "${LLD_LINK:-lld-link}" /machine:"$machine" /subsystem:console \
        /entry:mainCRTStartup /nodefaultlib /fixed /nxcompat /opt:ref \
        /out:"$dir/crt_float_pe$arch.exe" "$dir/float.obj" "$@" "$dir/kernel32.lib"
    printf '%s\n' "$dir/crt_float_pe$arch.exe"
done
