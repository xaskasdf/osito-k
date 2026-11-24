#!/bin/sh
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
x86_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
out_dir=${1:-"$x86_dir/build/test-seh-diagnostic"}
for bits in 32 64; do
    dir="$out_dir/pe$bits"
    mkdir -p "$dir"
    if [ "$bits" = 32 ]; then
        target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
        cp "$x86_dir/test/seh-diagnostic-kernel32.def" "$dir/kernel32.def"
    else
        target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64
        sed 's/@[0-9][0-9]*$//' "$x86_dir/test/seh-diagnostic-kernel32.def" > "$dir/kernel32.def"
    fi
    "${LLVM_DLLTOOL:-llvm-dlltool}" -m "$dll_machine" -k \
        -d "$dir/kernel32.def" -l "$dir/kernel32.lib"
    for case_id in 0 1 2 3 4 5 6 7; do
        "${CLANG:-clang}" --target="$target" -O2 -ffreestanding \
            -fno-builtin -fno-stack-protector -fno-omit-frame-pointer \
            -fno-ident -DSEH_DIAG_CASE="$case_id" \
            -c "$x86_dir/test/seh_diagnostic_pe.c" -o "$dir/diag$case_id.obj"
        "${LLD_LINK:-lld-link}" /machine:"$machine" /subsystem:console \
            /entry:mainCRTStartup /nodefaultlib /fixed /nxcompat /timestamp:0 \
            /out:"$dir/seh$bits-$case_id.exe" "$dir/diag$case_id.obj" "$dir/kernel32.lib"
    done
done
"${CLANG:-clang}" --target=x86_64-pc-windows-msvc -O2 -ffreestanding \
    -fno-builtin -fno-stack-protector -fno-ident -DFAULT_READ_LAUNCHER=1 \
    -c "$x86_dir/test/fault_read_pe.c" -o "$out_dir/launcher.obj"
"${LLD_LINK:-lld-link}" /machine:x64 /subsystem:console \
    /entry:mainCRTStartup /nodefaultlib /fixed /nxcompat /timestamp:0 \
    /out:"$out_dir/seh-launch.exe" "$out_dir/launcher.obj" "$out_dir/pe64/kernel32.lib"
