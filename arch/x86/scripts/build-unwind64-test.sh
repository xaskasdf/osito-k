#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
x86_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
out_dir=${1:-"$x86_dir/build/test-pe64"}

clang_bin=${CLANG:-clang}
link_bin=${LLD_LINK:-lld-link}
dlltool_bin=${LLVM_DLLTOOL:-llvm-dlltool}

mkdir -p "$out_dir"

"$dlltool_bin" -m i386:x86-64 -d "$x86_dir/test/pe64-kernel32.def" \
    -l "$out_dir/kernel32.lib"
"$dlltool_bin" -m i386:x86-64 -d "$x86_dir/test/pe64-ntdll.def" \
    -l "$out_dir/ntdll.lib"
"$dlltool_bin" -m i386:x86-64 -d "$x86_dir/test/pe64-msvcrt.def" \
    -l "$out_dir/msvcrt.lib"

"$clang_bin" --target=x86_64-pc-windows-msvc -O2 -fms-extensions \
    -fasync-exceptions \
    -ffreestanding \
    -fno-builtin -fno-stack-protector -fno-ident -c \
    "$x86_dir/test/unwind64_pe64.c" -o "$out_dir/unwind64_pe64.obj"
"$clang_bin" --target=x86_64-pc-windows-msvc -c \
    "$x86_dir/test/unwind64_fixture.S" -o "$out_dir/unwind64_fixture.obj"

"$link_bin" /machine:x64 /subsystem:console,6.1 /entry:mainCRTStartup \
    /nodefaultlib /fixed /opt:ref /out:"$out_dir/unwind64_pe64.exe" \
    "$out_dir/unwind64_pe64.obj" "$out_dir/unwind64_fixture.obj" \
    "$out_dir/kernel32.lib" "$out_dir/ntdll.lib" "$out_dir/msvcrt.lib"

printf '%s\n' "$out_dir/unwind64_pe64.exe"
