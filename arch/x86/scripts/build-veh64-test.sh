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

"$clang_bin" --target=x86_64-pc-windows-msvc -O2 -ffreestanding \
    -fno-builtin -fno-stack-protector -fno-ident -c \
    "$x86_dir/test/veh_pe64.c" -o "$out_dir/veh_pe64.obj"
"$clang_bin" --target=x86_64-pc-windows-msvc -O2 -ffreestanding \
    -fno-builtin -fno-stack-protector -fno-ident -c \
    "$x86_dir/test/veh_unhandled_pe64.c" \
    -o "$out_dir/veh_unhandled_pe64.obj"
"$clang_bin" --target=x86_64-pc-windows-msvc -O2 -ffreestanding \
    -fno-builtin -fno-stack-protector -fno-ident -c \
    "$x86_dir/test/veh_launcher_pe64.c" \
    -o "$out_dir/veh_launcher_pe64.obj"

"$link_bin" /machine:x64 /subsystem:console,6.1 /entry:mainCRTStartup \
    /nodefaultlib /fixed /opt:ref /out:"$out_dir/veh_pe64.exe" \
    "$out_dir/veh_pe64.obj" "$out_dir/kernel32.lib"
"$link_bin" /machine:x64 /subsystem:console,6.1 /entry:mainCRTStartup \
    /nodefaultlib /fixed /opt:ref \
    /out:"$out_dir/veh_unhandled_pe64.exe" \
    "$out_dir/veh_unhandled_pe64.obj"
"$link_bin" /machine:x64 /subsystem:console,6.1 /entry:mainCRTStartup \
    /nodefaultlib /fixed /opt:ref /out:"$out_dir/veh_launcher_pe64.exe" \
    "$out_dir/veh_launcher_pe64.obj" "$out_dir/kernel32.lib"

printf '%s\n' "$out_dir/veh_pe64.exe"
printf '%s\n' "$out_dir/veh_unhandled_pe64.exe"
printf '%s\n' "$out_dir/veh_launcher_pe64.exe"
