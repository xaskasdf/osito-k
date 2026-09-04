#!/bin/sh
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
x86_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
out_dir=${1:-"$x86_dir/build/test-pe32"}
mkdir -p "$out_dir"
for dll in kernel32 user32; do
    "${LLVM_DLLTOOL:-llvm-dlltool}" -m i386 -k \
        -d "$x86_dir/test/pe32-$dll.def" -l "$out_dir/$dll.lib"
done
"${CLANG:-clang}" --target=i686-pc-windows-msvc -O2 -ffreestanding \
    -fno-builtin -fno-stack-protector -fno-ident -c \
    "$x86_dir/test/callback_preemption_pe32.c" \
    -o "$out_dir/callback_preemption_pe32.obj"
"${LLD_LINK:-lld-link}" /machine:x86 /subsystem:console /entry:mainCRTStartup \
    /nodefaultlib /fixed /nxcompat /opt:ref \
    /out:"$out_dir/callback_preemption_pe32.exe" \
    "$out_dir/callback_preemption_pe32.obj" \
    "$out_dir/kernel32.lib" "$out_dir/user32.lib"
printf '%s\n' "$out_dir/callback_preemption_pe32.exe"
