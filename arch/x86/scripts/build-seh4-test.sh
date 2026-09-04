#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
x86_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
out_dir=${1:-"$x86_dir/build/test-pe32"}

clang_bin=${CLANG:-clang}
link_bin=${LLD_LINK:-lld-link}
dlltool_bin=${LLVM_DLLTOOL:-llvm-dlltool}

mkdir -p "$out_dir"

"$dlltool_bin" -m i386 -k -d "$x86_dir/test/pe32-kernel32.def" \
    -l "$out_dir/kernel32.lib"
"$dlltool_bin" -m i386 -k -d "$x86_dir/test/pe32-msvcrt.def" \
    -l "$out_dir/msvcrt.lib"

"$clang_bin" --target=i686-pc-windows-msvc -c \
    "$x86_dir/test/seh4_pe32.S" -o "$out_dir/seh4_pe32.obj"

"$link_bin" /machine:x86 /subsystem:console /entry:mainCRTStartup \
    /nodefaultlib /fixed /opt:ref /safeseh:no \
    /out:"$out_dir/seh4_pe32.exe" "$out_dir/seh4_pe32.obj" \
    "$out_dir/kernel32.lib" "$out_dir/msvcrt.lib"

printf '%s\n' "$out_dir/seh4_pe32.exe"
