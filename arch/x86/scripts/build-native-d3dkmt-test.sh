#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required}
sdk=${2:-} # Optional SDK Include/<version> directory, used without copying it.
msvc=${3:-} # Matching MSVC include directory for excpt.h/vcruntime.h.
if [[ -n $sdk && -z $msvc ]]; then
    printf '%s\n' 'SDK ABI validation also needs the MSVC include directory (argument 3).' >&2
    exit 2
fi
for bits in 32 64; do
    dir="$out/pe$bits"; mkdir -p "$dir"
    cat > "$dir/kernel32.def" <<'EOF'
LIBRARY KERNEL32.dll
EXPORTS
LoadLibraryA@4
GetProcAddress@8
FreeLibrary@4
GetStdHandle@4
WriteFile@20
ExitProcess@4
SetErrorMode@4
EOF
    if [[ $bits == 32 ]]; then
        target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
    else
        target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64
        sed -i 's/@[0-9][0-9]*$//' "$dir/kernel32.def"
    fi
    llvm-dlltool -m "$dll_machine" -k -d "$dir/kernel32.def" -l "$dir/kernel32.lib"
    clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
        -Wall -Wextra -Werror -c arch/x86/test/native_d3dkmt_pe.c -o "$dir/probe.obj"
    lld-link /entry:native_d3dkmt_entry /subsystem:console /nodefaultlib \
        /machine:"$machine" /fixed /timestamp:0 "/out:$dir/dxkmt$bits.exe" \
        "$dir/probe.obj" "$dir/kernel32.lib"
    clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
        -Wall -Wextra -Werror -DKMT_EXPECT_EMPTY \
        -c arch/x86/test/native_d3dkmt_pe.c -o "$dir/empty.obj"
    lld-link /entry:native_d3dkmt_entry /subsystem:console /nodefaultlib \
        /machine:"$machine" /fixed /timestamp:0 "/out:$dir/dxkmt$bits-empty.exe" \
        "$dir/empty.obj" "$dir/kernel32.lib"
    clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
        -Wall -Wextra -Werror -c arch/x86/test/native_kmt_enum_pe.c -o "$dir/enum.obj"
    lld-link /entry:native_kmt_enum_entry /subsystem:console /nodefaultlib \
        /machine:"$machine" /fixed /timestamp:0 "/out:$dir/enum$bits.exe" \
        "$dir/enum.obj" "$dir/kernel32.lib"
    if [[ -n $sdk ]]; then
        clang --target="$target" -ffreestanding -fms-extensions \
            -Wno-ignored-attributes -Wno-pragma-pack -Wno-nonportable-include-path \
            -DWIN32_LEAN_AND_MEAN -isystem "$msvc" \
            -isystem "$sdk/um" -isystem "$sdk/shared" -isystem "$sdk/ucrt" \
            -c arch/x86/test/native_d3dkmt_sdk_abi.c -o "$dir/sdk-abi.obj"
    fi
done
