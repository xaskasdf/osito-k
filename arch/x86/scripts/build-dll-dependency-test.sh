#!/bin/sh
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
x86_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
out_dir=${1:-"$x86_dir/build/test-dll-dependency"}
mkdir -p "$out_dir/pe64"
dir="$out_dir/pe64"
"${LLVM_DLLTOOL:-llvm-dlltool}" -m i386:x86-64 \
    -d "$x86_dir/test/dll-dependency-kernel32.def" -l "$dir/kernel32.lib"
"${CLANG:-clang}" --target=x86_64-pc-windows-msvc -O2 -ffreestanding \
    -fno-builtin -fno-stack-protector -fno-ident -c \
    "$x86_dir/test/dll_dependency_pe64.c" -o "$dir/dependency.obj"
"${LLD_LINK:-lld-link}" /machine:x64 /subsystem:console \
    /entry:mainCRTStartup /nodefaultlib /fixed /nxcompat /opt:ref /timestamp:0 \
    /out:"$dir/dependency64.exe" "$dir/dependency.obj" "$dir/kernel32.lib"
