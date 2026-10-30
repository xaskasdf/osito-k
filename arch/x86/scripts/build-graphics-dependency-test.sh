#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required}
for bits in 32 64; do
    dir="$out/pe$bits"; mkdir -p "$dir"
    cp arch/x86/test/wgl-backend-kernel32.def "$dir/kernel32.def"
    printf 'GetCurrentProcessId@0\nGetModuleHandleA@4\nFreeLibrary@4\n' >> "$dir/kernel32.def"
    if [[ $bits == 32 ]]; then
        target=i686-pc-windows-msvc; machine=x86; dll_machine=i386; entry='cycle_dll_main@12'
        parent_base=0x65000000; child_base=0x66000000
    else
        target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64; entry=cycle_dll_main
        parent_base=0x190000000; child_base=0x1a0000000
        sed -i 's/@[0-9][0-9]*$//' "$dir/kernel32.def"
    fi
    llvm-dlltool -m "$dll_machine" -k -d "$dir/kernel32.def" -l "$dir/kernel32.lib"
    for kind in child parent runner; do
        flags=(); [[ $kind != runner ]] && flags=(-D"CYCLE_${kind^^}")
        clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
            -Wall -Wextra -Werror "${flags[@]}" -c arch/x86/test/graphics_dependency_pe.c -o "$dir/$kind.obj"
    done
    lld-link /dll /nodefaultlib /machine:"$machine" "/entry:$entry" "/base:$child_base" \
        "/out:$dir/cycle-child.dll" /export:cycle_peer "$dir/child.obj" "$dir/kernel32.lib"
    lld-link /dll /nodefaultlib /machine:"$machine" "/entry:$entry" "/base:$parent_base" \
        "/out:$dir/cycle-parent.dll" /export:cycle_value "$dir/parent.obj" "$dir/cycle-child.lib" "$dir/kernel32.lib"
    lld-link /entry:cycle_entry /subsystem:console /nodefaultlib /machine:"$machine" \
        /fixed "/out:$dir/cycle$bits.exe" "$dir/runner.obj" "$dir/kernel32.lib"
done
