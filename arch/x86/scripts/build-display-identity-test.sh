#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required}
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
    for mode in windows generic intel virtio; do
        flags=()
        if [[ $mode == generic ]]; then
            flags+=('-DDISPLAY_EXPECT_ID=""' '-DDISPLAY_EXPECT_DESCRIPTION="UEFI GOP Display"')
        elif [[ $mode == intel ]]; then
            flags+=('-DDISPLAY_EXPECT_ID="PCI\\VEN_8086&DEV_3E9B"' '-DDISPLAY_EXPECT_DESCRIPTION="Intel UHD Graphics 630"')
        elif [[ $mode == virtio ]]; then
            flags+=('-DDISPLAY_EXPECT_ID="PCI\\VEN_1AF4&DEV_1050"' '-DDISPLAY_EXPECT_DESCRIPTION="VirtIO GPU Display"')
        fi
        clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
            -Wall -Wextra -Werror "${flags[@]}" \
            -c arch/x86/test/display_identity_pe.c -o "$dir/identity.obj"
        lld-link /entry:display_identity_entry /subsystem:console /nodefaultlib \
            /machine:"$machine" /fixed /timestamp:0 "/out:$dir/identity$bits-$mode.exe" \
            "$dir/identity.obj" "$dir/kernel32.lib"
    done
done
