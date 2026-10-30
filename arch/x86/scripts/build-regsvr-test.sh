#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required}
for bits in 32 64; do
    dir="$out/pe$bits"; mkdir -p "$dir"
    if [[ $bits == 32 ]]; then
        target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
        printf 'LIBRARY kernel32.dll\nEXPORTS\nCreateFileA@28\nWriteFile@20\nCloseHandle@4\nGetEnvironmentVariableA@12\n' > "$dir/kernel32.def"
        printf 'LIBRARY ole32.dll\nEXPORTS\nCoInitializeEx@8\nCoUninitialize@0\n' > "$dir/ole32.def"
        printf 'EXPORTS\nDllRegisterServer=_DllRegisterServer@0\nDllUnregisterServer=_DllUnregisterServer@0\nDllInstall=_DllInstall@8\n' > "$dir/fixture.def"
    else
        target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64
        sed 's/@[0-9][0-9]*$//' "$out/pe32/kernel32.def" > "$dir/kernel32.def"
        printf 'LIBRARY ole32.dll\nEXPORTS\nCoInitializeEx\nCoUninitialize\n' > "$dir/ole32.def"
        printf 'EXPORTS\nDllRegisterServer\nDllUnregisterServer\nDllInstall\n' > "$dir/fixture.def"
    fi
    for dll in kernel32 ole32; do
        llvm-dlltool -m "$dll_machine" -k -d "$dir/$dll.def" -l "$dir/$dll.lib"
    done
    for variant in fixture missing; do
        extra=(); def=()
        if [[ $variant == missing ]]; then extra=(-DMISSING_EXPORTS); else def=("/def:$dir/fixture.def"); fi
        clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
            -Wall -Wextra -Werror "${extra[@]}" -c arch/x86/test/regsvr_fixture.c -o "$dir/$variant.obj"
        lld-link /dll /entry:regsvr_fixture_main /nodefaultlib /machine:"$machine" \
            "${def[@]}" "/out:$dir/reg$variant.dll" "$dir/$variant.obj" "$dir/kernel32.lib" "$dir/ole32.lib"
    done
    cp "$dir/regfixture.dll" "$dir/reg fixture.dll"
    printf 'LIBRARY kernel32.dll\nEXPORTS\nExitProcess@4\nGetStdHandle@4\nWriteFile@20\nGetModuleFileNameA@12\nGetSystemDirectoryW@8\nSetCurrentDirectoryA@4\nSetEnvironmentVariableA@8\nDeleteFileA@4\nCreateProcessW@40\nWaitForSingleObject@8\nGetExitCodeProcess@8\nCloseHandle@4\nCreateFileA@28\nReadFile@20\nTerminateProcess@8\n' > "$dir/probe-kernel32.def"
    if [[ $bits == 64 ]]; then sed -i 's/@[0-9][0-9]*$//' "$dir/probe-kernel32.def"; fi
    llvm-dlltool -m "$dll_machine" -k -d "$dir/probe-kernel32.def" -l "$dir/probe-kernel32.lib"
    clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
        -Wall -Wextra -Werror -c arch/x86/test/regsvr_pe.c -o "$dir/probe.obj"
    lld-link /entry:regsvr_test_entry /subsystem:console /nodefaultlib /machine:"$machine" \
        /fixed "/out:$dir/regsvrtest$bits.exe" "$dir/probe.obj" "$dir/probe-kernel32.lib"
done
