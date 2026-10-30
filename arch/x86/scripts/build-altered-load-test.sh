#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required}
for bits in 32 64; do
    dir="$out/pe$bits"; mkdir -p "$dir/plugin" "$dir/cwd"
    if [[ $bits == 32 ]]; then
        target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
    else
        target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64
    fi
    printf 'LIBRARY kernel32.dll\nEXPORTS\nExitProcess@4\nGetStdHandle@4\nWriteFile@20\nGetModuleFileNameA@12\nLoadLibraryExA@12\nGetProcAddress@8\nGetModuleHandleA@4\nFreeLibrary@4\nSetCurrentDirectoryA@4\nGetLastError@0\nSetLastError@4\n' > "$dir/kernel32.def"
    if [[ $bits == 64 ]]; then sed -i 's/@[0-9][0-9]*$//' "$dir/kernel32.def"; fi
    llvm-dlltool -m "$dll_machine" -k -d "$dir/kernel32.def" -l "$dir/kernel32.lib"
    printf 'EXPORTS\nchild=altered_value\n' > "$dir/dep.def"
    printf 'EXPORTS\naltered_value\n' > "$dir/parent.def"
    for value in 0x1111 0x2222; do
        if [[ $value == 0x1111 ]]; then dll="$dir/altered-dep.dll"; else dll="$dir/plugin/altered-dep.dll"; fi
        clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
            -Wall -Wextra -Werror -DVALUE="$value" -c arch/x86/test/altered_load_dll.c -o "$dir/dep.obj"
        lld-link /dll /noentry /nodefaultlib /machine:"$machine" "/def:$dir/dep.def" "/out:$dll" "$dir/dep.obj"
    done
    cp "$dir/plugin/altered-dep.dll" "$dir/cwd/cwd-only.dll"
    clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
        -Wall -Wextra -Werror -DPARENT_DLL -c arch/x86/test/altered_load_dll.c -o "$dir/parent.obj"
    lld-link /dll /noentry /nodefaultlib /machine:"$machine" "/def:$dir/parent.def" "/out:$dir/plugin/parent.dll" "$dir/parent.obj" "$dir/altered-dep.lib"
    clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
        -Wall -Wextra -Werror -c arch/x86/test/altered_load_pe.c -o "$dir/probe.obj"
    lld-link /entry:altered_load_entry /subsystem:console /nodefaultlib /machine:"$machine" \
        /fixed "/out:$dir/altered$bits.exe" "$dir/probe.obj" "$dir/kernel32.lib"
    for case in 0 1 2 3 4 5; do
        clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
            -Wall -Wextra -Werror -DISOLATED_CASE="$case" -c arch/x86/test/altered_load_pe.c -o "$dir/probe-$case.obj"
        lld-link /entry:altered_load_entry /subsystem:console /nodefaultlib /machine:"$machine" \
            /fixed "/out:$dir/altered$bits-$case.exe" "$dir/probe-$case.obj" "$dir/kernel32.lib"
    done
done
