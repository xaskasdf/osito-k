#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required}
for bits in 32 64; do
    dir="$out/pe$bits"; mkdir -p "$dir"
    if [[ $bits == 32 ]]; then
        target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
        cp arch/x86/test/wgl-backend-kernel32.def "$dir/kernel32.def"
    else
        target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64
        sed 's/@[0-9][0-9]*$//' arch/x86/test/wgl-backend-kernel32.def > "$dir/kernel32.def"
    fi
    llvm-dlltool -m "$dll_machine" -k -d "$dir/kernel32.def" -l "$dir/kernel32.lib"
    for item in rpc_uuid reg_enum_legacy; do
        entry=rpc_uuid_entry; leaf=rpcuuid
        if [[ $item == reg_enum_legacy ]]; then entry=reg_enum_entry; leaf=regenum; fi
        clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
            -Wall -Wextra -Werror -c "arch/x86/test/${item}_pe.c" -o "$dir/$leaf.obj"
        lld-link /entry:"$entry" /subsystem:console /nodefaultlib /machine:"$machine" \
            /fixed "/out:$dir/$leaf$bits.exe" "$dir/$leaf.obj" "$dir/kernel32.lib"
    done
    clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
        -Wall -Wextra -Werror '-DREG_ENUM_PROBE_NAME="alpha-longname"' \
        -c arch/x86/test/reg_enum_legacy_pe.c -o "$dir/regenumlower.obj"
    lld-link /entry:reg_enum_entry /subsystem:console /nodefaultlib /machine:"$machine" \
        /fixed "/out:$dir/regenumlower$bits.exe" "$dir/regenumlower.obj" "$dir/kernel32.lib"
done
