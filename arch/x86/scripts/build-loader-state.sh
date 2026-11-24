#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required}
for bits in 32 64;do
    dir="$out/pe$bits";mkdir -p "$dir"
    cat > "$dir/kernel32.def" <<'EOF'
LIBRARY KERNEL32.dll
EXPORTS
ExitProcess@4
ExitThread@4
GetStdHandle@4
WriteFile@20
LoadLibraryA@4
GetProcAddress@8
GetModuleHandleA@4
FreeLibrary@4
GetLastError@0
SetLastError@4
CreateThread@24
WaitForSingleObject@8
GetExitCodeThread@8
CloseHandle@4
EOF
    cat > "$dir/ntdll.def" <<'EOF'
LIBRARY ntdll.dll
EXPORTS
RtlDllShutdownInProgress@0
RtlIsThreadWithinLoaderCallout@0
EOF
    if [[ $bits == 32 ]];then
        target=i686-pc-windows-msvc;machine=x86;dll_machine=i386;entry=loader_state_dll_main@12
    else
        target=x86_64-pc-windows-msvc;machine=x64;dll_machine=i386:x86-64;entry=loader_state_dll_main
        sed -i 's/@[0-9][0-9]*$//' "$dir/kernel32.def" "$dir/ntdll.def"
    fi
    for dll in kernel32 ntdll;do llvm-dlltool -m "$dll_machine" -k -d "$dir/$dll.def" -l "$dir/$dll.lib";done
    for variant in good fail;do
        failing=0;name=loader-state;[[ $variant == fail ]] && failing=1 && name=loader-state-fail
        clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector -Wall -Wextra -Werror \
            -DFAIL_ATTACH="$failing" -c arch/x86/test/loader_state_library.c -o "$dir/$variant.obj"
        lld-link /dll /nodefaultlib /machine:"$machine" "/entry:$entry" /timestamp:0 \
            "/out:$dir/$name.dll" "$dir/$variant.obj" "$dir/kernel32.lib" "$dir/ntdll.lib"
    done
    for variant in normal explicit;do
        extra=();suffix='';[[ $variant == explicit ]] && extra=(-DEXPLICIT_SHUTDOWN) && suffix=-explicit
        clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector -Wall -Wextra -Werror "${extra[@]}" \
            -c arch/x86/test/loader_state_pe.c -o "$dir/probe-$variant.obj"
        lld-link /entry:loader_state_entry /subsystem:console /nodefaultlib /machine:"$machine" /fixed /timestamp:0 \
            "/out:$dir/loader-state$bits$suffix.exe" "$dir/probe-$variant.obj" "$dir/kernel32.lib" "$dir/ntdll.lib"
    done
    for variant in a b tls;do
        extra=("-DSTATE_LABEL=\"$variant\"");link_entry=("/entry:$entry")
        [[ $variant == b ]] && extra+=(-DREENTER_SHUTDOWN)
        [[ $variant == tls ]] && link_entry=(/noentry)
        clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector -Wall -Wextra -Werror \
            "${extra[@]}" -c arch/x86/test/loader_state_library.c -o "$dir/$variant.obj"
        lld-link /dll /nodefaultlib /machine:"$machine" "${link_entry[@]}" /timestamp:0 \
            "/out:$dir/loader-state-$variant.dll" "$dir/$variant.obj" "$dir/kernel32.lib" "$dir/ntdll.lib"
    done
    for variant in multi locked;do
        extra=();[[ $variant == locked ]] && extra=(-DSHUTDOWN_LOCKED)
        clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector -Wall -Wextra -Werror "${extra[@]}" \
            -c arch/x86/test/loader_state_multi.c -o "$dir/probe-$variant.obj"
        lld-link /entry:loader_multi_entry /subsystem:console /nodefaultlib /machine:"$machine" /fixed /timestamp:0 \
            "/out:$dir/loader-state$bits-$variant.exe" "$dir/probe-$variant.obj" "$dir/kernel32.lib" "$dir/ntdll.lib"
    done
done
