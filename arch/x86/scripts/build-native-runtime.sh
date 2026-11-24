#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory}
sdk=${2:?local SDK include/version directory}
msvc=${3:?local MSVC include directory}
for bits in 32 64; do
    dir="$out/pe$bits"; mkdir -p "$dir"
    target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
    if [[ $bits == 64 ]]; then target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64; fi
    cat > "$dir/kernel32.def" <<'EOF'
LIBRARY KERNEL32.dll
EXPORTS
LoadLibraryA@4
GetProcAddress@8
GetModuleHandleExA@12
GetModuleFileNameA@12
GetSystemDirectoryA@8
FreeLibrary@4
GetLastError@0
GetStdHandle@4
WriteFile@20
ExitProcess@4
SetErrorMode@4
EOF
    [[ $bits == 64 ]] && sed -i 's/@[0-9][0-9]*$//' "$dir/kernel32.def"
    llvm-dlltool -m "$dll_machine" -k -d "$dir/kernel32.def" -l "$dir/kernel32.lib"
    printf 'LIBRARY MSVCRT.dll\nEXPORTS\nmemset\n' > "$dir/msvcrt.def"
    llvm-dlltool -m "$dll_machine" -d "$dir/msvcrt.def" -l "$dir/msvcrt.lib"
    for mode in load factory host; do
        extra=(); [[ $mode == load ]] && extra=(-DRUNTIME_LOAD_ONLY)
        [[ $mode == host ]] && extra=(-DRUNTIME_HOST)
        clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
            -Wall -Wextra -Werror -Wno-ignored-attributes -Wno-pragma-pack -Wno-nonportable-include-path \
            -isystem "$msvc" -isystem "$sdk/um" -isystem "$sdk/shared" -isystem "$sdk/ucrt" \
            "${extra[@]}" -c arch/x86/test/native_runtime_pe.c -o "$dir/$mode.obj"
        lld-link /entry:native_runtime_entry /subsystem:console /nodefaultlib /machine:"$machine" /timestamp:0 /fixed \
            "/out:$dir/runtime$bits-$mode.exe" "$dir/$mode.obj" "$dir/kernel32.lib" "$dir/msvcrt.lib"
    done
done
