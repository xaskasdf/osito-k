#!/bin/sh
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
x86_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
out_dir=${1:-"$x86_dir/build/test-cxx-catch"}
mkdir -p "$out_dir"
"${LLVM_DLLTOOL:-llvm-dlltool}" -m i386 -k \
    -d "$x86_dir/test/ddraw-display-kernel32.def" -l "$out_dir/kernel32.lib"
"${LLVM_DLLTOOL:-llvm-dlltool}" -m i386 -k \
    -d "$x86_dir/test/cxx-catch-msvcrt.def" -l "$out_dir/msvcrt.lib"
"${LLVM_DLLTOOL:-llvm-dlltool}" -m i386 -k \
    -d "$x86_dir/test/cxx-catch-user32.def" -l "$out_dir/user32.lib"
"${CLANG:-clang}" --target=i686-pc-windows-msvc -O2 -ffreestanding \
    -fno-builtin -fno-stack-protector -c "$x86_dir/test/cxx_catch_pe32.c" \
    -o "$out_dir/cxx_catch.obj"
"${CLANG:-clang}" --target=i686-pc-windows-msvc -c \
    "$x86_dir/test/cxx_catch_pe32.S" -o "$out_dir/cxx_fixture.obj"
"${LLD_LINK:-lld-link}" /machine:x86 /subsystem:console /entry:mainCRTStartup \
    /nodefaultlib /fixed /opt:ref /safeseh:no /out:"$out_dir/cxx_catch_pe32.exe" \
    "$out_dir/cxx_catch.obj" "$out_dir/cxx_fixture.obj" \
    "$out_dir/kernel32.lib" "$out_dir/msvcrt.lib" "$out_dir/user32.lib"
printf '%s\n' "$out_dir/cxx_catch_pe32.exe"
