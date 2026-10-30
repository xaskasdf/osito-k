#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required}
for bits in 32 64; do
    dir="$out/pe$bits";mkdir -p "$dir"
    cat > "$dir/kernel32.def" <<'EOF'
LIBRARY KERNEL32.dll
EXPORTS
ExitProcess@4
GetStdHandle@4
WriteFile@20
LoadLibraryA@4
GetProcAddress@8
GetModuleHandleA@4
FreeLibrary@4
GetLastError@0
SetLastError@4
GetCurrentProcessId@0
CreateMutexA@12
OpenMutexA@12
CloseHandle@4
DisableThreadLibraryCalls@4
TlsAlloc@0
TlsFree@4
TlsSetValue@8
TlsGetValue@4
CreateThread@24
WaitForSingleObject@8
GetExitCodeThread@8
EOF
    if [[ $bits == 32 ]]; then
        target=i686-pc-windows-msvc;machine=x86;dll_machine=i386;entry='lifecycle_dll_main@12'
    else
        target=x86_64-pc-windows-msvc;machine=x64;dll_machine=i386:x86-64;entry=lifecycle_dll_main
        sed -i 's/@[0-9][0-9]*$//' "$dir/kernel32.def"
    fi
    llvm-dlltool -m "$dll_machine" -k -d "$dir/kernel32.def" -l "$dir/kernel32.lib"
    for kind in good fail static-good static-fail;do
        failing=0;[[ $kind == *fail ]] && failing=1
        extra=();[[ $kind == static-* ]] && extra=(-DSTATIC_TLS=1)
        clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector -Wall -Wextra -Werror \
            -DFAIL_ATTACH="$failing" "${extra[@]}" -c arch/x86/test/dll_lifecycle_library.c -o "$dir/$kind.obj"
        lld-link /dll /nodefaultlib /machine:"$machine" "/entry:$entry" \
            "/out:$dir/lifecycle-$kind.dll" "$dir/$kind.obj" "$dir/kernel32.lib"
    done
    clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector -Wall -Wextra -Werror \
        -c arch/x86/test/dll_lifecycle_pe.c -o "$dir/probe.obj"
    lld-link /entry:lifecycle_entry /subsystem:console /nodefaultlib /machine:"$machine" /fixed \
        "/out:$dir/lifecycle$bits.exe" "$dir/probe.obj" "$dir/kernel32.lib"
done
