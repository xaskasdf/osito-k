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
    for version in 62 63 100; do
        clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector -Wall -Wextra -Werror \
            -DVERSION_TARGET="$version" -c arch/x86/test/version_pe.c -o "$dir/v$version.obj"
        manifest=(/manifest:no)
        if [[ $version != 62 ]]; then
            guid='{1f676c76-80e1-4239-95bb-83d0f6d0da78}'
            if [[ $version == 100 ]]; then guid='{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}'; fi
            cat > "$dir/v$version.manifest" <<EOF
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
<compatibility xmlns="urn:schemas-microsoft-com:compatibility.v1"><application>
<supportedOS Id="$guid"/>
</application></compatibility>
</assembly>
EOF
            manifest=(/manifest:embed "/manifestinput:$dir/v$version.manifest")
        fi
        lld-link /entry:version_probe_entry /subsystem:console /nodefaultlib /machine:"$machine" \
            /fixed "${manifest[@]}" "/out:$dir/version$bits-$version.exe" "$dir/v$version.obj" "$dir/kernel32.lib"
    done
done
