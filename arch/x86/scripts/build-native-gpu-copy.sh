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
GetStdHandle@4
WriteFile@20
ExitProcess@4
Sleep@4
GetCurrentProcessId@0
GetModuleFileNameA@12
CreateProcessA@40
WaitForSingleObject@8
GetExitCodeProcess@8
CloseHandle@4
CreateEventA@16
DuplicateHandle@28
CreateThread@24
SetEvent@4
GetExitCodeThread@8
TerminateThread@8
EOF
    if [[ $bits == 32 ]]; then
        target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
    else
        target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64
        sed -i 's/@[0-9][0-9]*$//' "$dir/kernel32.def"
    fi
    llvm-dlltool -m "$dll_machine" -k -d "$dir/kernel32.def" -l "$dir/kernel32.lib"
    for mode in ${GPUCOPY_MODES:-copy exit root root-exit sync monitored monitored-api monitored-cpu-wait monitored-cpu-wait-exit monitored-broadcast lock2 lock2-sync lock2-exit paging residency offer blocking-lock}; do
        flags=()
        if [[ $mode == lock2 || $mode == lock2-sync || $mode == lock2-exit ]]; then flags+=(-DGPUCOPY_LOCK2); fi
        if [[ $mode == lock2-sync ]]; then flags+=(-DGPUCOPY_SYNC); fi
        if [[ $mode == paging ]]; then flags+=(-DGPUCOPY_SYNC -DGPUCOPY_PAGING); fi
        if [[ $mode == residency ]]; then flags+=(-DGPUCOPY_SYNC -DGPUCOPY_LOCK2 -DGPUCOPY_RESIDENCY); fi
        if [[ $mode == offer ]]; then flags+=(-DGPUCOPY_SYNC -DGPUCOPY_LOCK2 -DGPUCOPY_OFFER); fi
        if [[ $mode == lock2-exit ]]; then flags+=(-DGPUCOPY_EXIT_PENDING); fi
        if [[ $mode == exit || $mode == root-exit ]]; then flags+=(-DGPUCOPY_EXIT_PENDING); fi
        if [[ $mode == root || $mode == root-exit ]]; then flags+=(-DGPUCOPY_ROOT); fi
        if [[ $mode == sync ]]; then flags+=(-DGPUCOPY_SYNC); fi
        if [[ $mode == blocking-lock ]]; then flags+=(-DGPUCOPY_SYNC -DGPUCOPY_BLOCKING_LOCK); fi
        if [[ $mode == monitored ]]; then flags+=(-DGPUCOPY_SYNC -DGPUCOPY_MONITORED); fi
        if [[ $mode == monitored-api ]]; then flags+=(-DGPUCOPY_SYNC -DGPUCOPY_MONITORED -DGPUCOPY_MONITORED_APIS); fi
        if [[ $mode == monitored-cpu-wait || $mode == monitored-cpu-wait-exit ]]; then
            flags+=(-DGPUCOPY_SYNC -DGPUCOPY_MONITORED -DGPUCOPY_MONITORED_APIS -DGPUCOPY_CPU_WAIT)
        fi
        if [[ $mode == monitored-cpu-wait-exit ]]; then flags+=(-DGPUCOPY_CPU_WAIT_EXIT); fi
        if [[ $mode == monitored-broadcast ]]; then
            flags+=(-DGPUCOPY_SYNC -DGPUCOPY_MONITORED -DGPUCOPY_MONITORED_APIS -DGPUCOPY_CPU_WAIT -DGPUCOPY_BROADCAST)
        fi
        clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
            -Wall -Wextra -Werror "${flags[@]}" -c arch/x86/test/native_gpu_copy_pe.c -o "$dir/$mode.obj"
        lld-link /entry:native_gpu_copy_entry /subsystem:console /nodefaultlib \
            /machine:"$machine" /fixed /timestamp:0 "/out:$dir/gpucopy$bits-$mode.exe" \
            "$dir/$mode.obj" "$dir/kernel32.lib"
    done
done
