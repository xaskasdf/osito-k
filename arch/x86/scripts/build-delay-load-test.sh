#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory};sdk=${2:?SDK includes};msvc=${3:?MSVC includes}
for bits in 32 64; do
    dir="$out/pe$bits";mkdir -p "$dir"
    target=i686-pc-windows-msvc;machine=x86;dll_machine=i386;export=DelayOrdinal=_DelayOrdinal@0;named=DelayValue=_DelayValue@0
    if [[ $bits == 64 ]];then target=x86_64-pc-windows-msvc;machine=x64;dll_machine=i386:x86-64;export=DelayOrdinal;named=DelayValue;fi
    cp arch/x86/test/wgl-backend-kernel32.def "$dir/kernel32.def"
    printf 'FreeLibrary@4\nAddVectoredExceptionHandler@8\n' >> "$dir/kernel32.def"
    [[ $bits == 64 ]] && sed -i 's/@[0-9][0-9]*$//' "$dir/kernel32.def"
    llvm-dlltool -m "$dll_machine" -k -d "$dir/kernel32.def" -l "$dir/kernel32.lib"
    clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
        -Wall -Wextra -Werror -Wno-ignored-attributes -Wno-pragma-pack -Wno-nonportable-include-path \
        -isystem "$msvc" -isystem "$sdk/um" -isystem "$sdk/shared" -isystem "$sdk/ucrt" \
        -c arch/x86/test/delay_load_pe.c -o "$dir/probe.obj"
    lld-link /entry:delay_load_entry /subsystem:console /nodefaultlib /machine:"$machine" /fixed /timestamp:0 \
        "/out:$dir/delay$bits.exe" "$dir/probe.obj" "$dir/kernel32.lib"
    clang --target="$target" -O2 -ffreestanding -fno-stack-protector -c arch/x86/test/delay_load_target.c -o "$dir/target.obj"
    lld-link /dll /noentry /nodefaultlib /machine:"$machine" /timestamp:0 "/out:$dir/delay-target.dll" \
        "/export:$export,@7,NONAME" "/export:$named,@5" "$dir/target.obj"
done
