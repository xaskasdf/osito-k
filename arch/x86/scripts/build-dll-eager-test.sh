#!/bin/sh
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
x86_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
out_dir=${1:-"$x86_dir/build/test-dll-eager"}
mkdir -p "$out_dir/pe64"
dir="$out_dir/pe64"
for module in kernel32 tier0 vstdlib; do
    "${LLVM_DLLTOOL:-llvm-dlltool}" -m i386:x86-64 \
        -d "$x86_dir/test/dll-eager-$module.def" -l "$dir/$module.lib"
done
"${CLANG:-clang}" --target=x86_64-pc-windows-msvc -O2 -ffreestanding \
    -fno-builtin -fno-stack-protector -fno-ident -c \
    "$x86_dir/test/dll_eager_dependency_pe64.c" -o "$dir/eager.obj"
"${LLD_LINK:-lld-link}" /machine:x64 /subsystem:console \
    /entry:mainCRTStartup /nodefaultlib /fixed /nxcompat /opt:ref /timestamp:0 \
    /out:"$dir/eager64.exe" "$dir/eager.obj" "$dir/kernel32.lib" "$dir/tier0.lib" "$dir/vstdlib.lib"
