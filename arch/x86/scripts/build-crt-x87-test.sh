#!/bin/sh
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
x86_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
out_dir=${1:-"$x86_dir/build/test-crt-x87"}
mkdir -p "$out_dir"
"${LLVM_DLLTOOL:-llvm-dlltool}" -m i386 -k \
    -d "$x86_dir/test/crt-fp-kernel32.def" -l "$out_dir/kernel32.lib"
"${CLANG:-clang}" --target=i686-pc-windows-msvc -O2 -msse2 \
    -ffreestanding -fno-builtin -fno-stack-protector -fno-ident -c \
    "$x86_dir/test/crt_x87_pe32.c" -o "$out_dir/crt_x87.obj"
"${LLD_LINK:-lld-link}" /machine:x86 /subsystem:console \
    /entry:mainCRTStartup /nodefaultlib /fixed /nxcompat /opt:ref \
    /out:"$out_dir/crt_x87_pe32.exe" "$out_dir/crt_x87.obj" "$out_dir/kernel32.lib"
printf '%s\n' "$out_dir/crt_x87_pe32.exe"
