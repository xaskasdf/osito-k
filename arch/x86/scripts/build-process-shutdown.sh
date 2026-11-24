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
GetStdHandle@4
WriteFile@20
LoadLibraryA@4
GetProcAddress@8
GetModuleHandleA@4
GetLastError@0
SetLastError@4
GetCurrentThreadId@0
GetCurrentProcess@0
TerminateProcess@8
OpenThread@12
Sleep@4
CreateEventA@16
SetEvent@4
CreateThread@24
WaitForSingleObject@8
GetExitCodeThread@8
CloseHandle@4
GetModuleFileNameA@12
CreateProcessA@40
GetExitCodeProcess@8
EOF
    cat > "$dir/ntdll.def" <<'EOF'
LIBRARY ntdll.dll
EXPORTS
RtlDllShutdownInProgress@0
RtlIsThreadWithinLoaderCallout@0
EOF
    if [[ $bits == 32 ]];then
        target=i686-pc-windows-msvc;machine=x86;dll_machine=i386;entry=process_shutdown_dll_main@12
    else
        target=x86_64-pc-windows-msvc;machine=x64;dll_machine=i386:x86-64;entry=process_shutdown_dll_main
        sed -i 's/@[0-9][0-9]*$//' "$dir/kernel32.def" "$dir/ntdll.def"
    fi
    for dll in kernel32 ntdll;do llvm-dlltool -m "$dll_machine" -k -d "$dir/$dll.def" -l "$dir/$dll.lib";done
    clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector -Wall -Wextra \
        -c arch/x86/test/process_shutdown_library.c -o "$dir/library.obj"
    lld-link /dll /nodefaultlib /machine:"$machine" "/entry:$entry" /timestamp:0 \
        "/out:$dir/process-shutdown.dll" "$dir/library.obj" "$dir/kernel32.lib" "$dir/ntdll.lib"
    for variant in main worker forced busy-main busy-worker;do
        extra=();[[ $variant == *worker ]] && extra=(-DSHUTDOWN_WORKER)
        [[ $variant == busy-* ]] && extra+=(-DSHUTDOWN_BUSY)
        [[ $variant == forced ]] && extra=(-DSHUTDOWN_FORCED)
        clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector -Wall -Wextra "${extra[@]}" \
            -c arch/x86/test/process_shutdown_pe.c -o "$dir/$variant.obj"
        lld-link /entry:process_shutdown_entry /subsystem:console /nodefaultlib /machine:"$machine" /fixed /timestamp:0 \
            "/out:$dir/shutdown$bits-$variant.exe" "$dir/$variant.obj" "$dir/kernel32.lib" "$dir/ntdll.lib"
    done
    clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector -Wall -Wextra \
        -c arch/x86/test/process_shutdown_parent.c -o "$dir/parent.obj"
    lld-link /entry:shutdown_parent_entry /subsystem:console /nodefaultlib /machine:"$machine" /fixed /timestamp:0 \
        "/out:$dir/shutdown$bits-parent.exe" "$dir/parent.obj" "$dir/kernel32.lib" "$dir/ntdll.lib"
done
