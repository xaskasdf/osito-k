#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$repo"
if [[ -z ${OSITO_TEST_DISK_DIR:-} ]]; then
    exec bash arch/x86/scripts/qemu-test-disks.sh run -- bash "$0" "$@"
fi
out=${1:-arch/x86/build/ssh-qemu}
test ! -e "$out"
mkdir -p "$out"
out=$(realpath "$out")
cp "${OSITO_SSH_TEST_KERNEL:-arch/x86/build/kernel.elf}" "$out/kernel.elf"
cp arch/x86/build/boot.efi "$out/boot.efi"
work=$OSITO_TEST_DISK_DIR
ssh-keygen -q -t ed25519 -N '' -f "$work/client"
ssh-keygen -q -t ed25519 -N '' -f "$work/wrong"
truncate -s 128M "$work/esp.img"
mformat -i "$work/esp.img" -F ::
mmd -i "$work/esp.img" ::/EFI ::/EFI/BOOT
mcopy -i "$work/esp.img" "$out/boot.efi" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$work/esp.img" "$out/kernel.elf" ::/EFI/BOOT/kernel.elf
truncate -s 128M "$work/ssh-test.img"
append_v2=${OSITO_SSH_TEST_FILE_APPEND_FS2:-0}
if [[ $append_v2 == 1 ]]; then
    test "${OSITO_SSH_TEST_FILE_APPEND:-0}" == 1
    tools/ositofs/mkfs.ositofs "$work/ssh-test.img" --label SSHTest \
        --block-size 65536 --file-slots 4096
else
    tools/ositofs/mkfs.ositofs3 "$work/ssh-test.img" --label SSHTest --inodes 4096
fi
write_boot_fixture() {
    if [[ $append_v2 == 1 ]]; then
        tools/ositofs/ositofs-write "$work/ssh-test.img" "$1" --name "${2#/}"
    else
        tools/ositofs/ositofs-write3 "$work/ssh-test.img" "$1" --dest "$2"
    fi
}
touch "$work/noautoload"
write_boot_fixture "$work/noautoload" /.noautoload
write_boot_fixture "$work/client.pub" /.ssh_authorized_keys
write_boot_fixture "$out/kernel.elf" /next.elf
if [[ ${OSITO_SSH_TEST_HWBP_TRACE:-0} == 1 ]]; then
    bash arch/x86/scripts/build-hwbp-trace-test.sh "$out/hwbp-trace"
    for bits in 32 64; do
        write_boot_fixture "$out/hwbp-trace/trace$bits.exe" "/trace$bits.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_WSASEND_FULL:-0} == 1 ]]; then
    bash arch/x86/scripts/build-winsock-send-completion-test.sh "$out/wsasend-full"
    for bits in 32 64; do
        write_boot_fixture "$out/wsasend-full/send$bits.exe" "/send$bits.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_WSASEND_QUEUE:-0} == 1 ]]; then
    bash arch/x86/scripts/build-winsock-send-queue-test.sh "$out/wsasend-queue"
    for bits in 32 64; do
        write_boot_fixture "$out/wsasend-queue/queue$bits.exe" "/queue$bits.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_CHILD_EXIT:-0} == 1 ]]; then
    bash arch/x86/scripts/build-child-exit-test.sh "$out/child-exit"
    for bits in 32 64; do
        write_boot_fixture "$out/child-exit/pe$bits/exit$bits.exe" "/exit$bits.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_CHILD_CAPACITY:-0} == 1 ]]; then
    bash arch/x86/scripts/build-child-capacity-test.sh "$out/child-capacity"
    for bits in 32 64; do
        write_boot_fixture "$out/child-capacity/capacity$bits.exe" "/capacity$bits.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_FILE_APPEND:-0} == 1 ]]; then
    bash arch/x86/scripts/build-file-append-test.sh "$out/append"
    for bits in 32 64; do
        write_boot_fixture "$out/append/pe$bits/append$bits.exe" "/append$bits.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_FILE_POSITION:-0} == 1 ]]; then
    bash arch/x86/scripts/build-file-position-test.sh "$out/position"
    for bits in 32 64; do
        write_boot_fixture "$out/position/pe$bits/position$bits.exe" "/position$bits.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_IOCP_DEADLINE:-0} == 1 ]]; then
    bash arch/x86/scripts/build-iocp-deadline-test.sh "$out/deadline"
    for bits in 32 64; do
        tools/ositofs/ositofs-write3 "$work/ssh-test.img" \
            "$out/deadline/pe$bits/deadline$bits.exe" --dest "/deadline$bits.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_VNET_PRIVATE:-0} == 1 ]]; then
    for bits in 32 64; do
        if [[ $bits == 32 ]]; then
            target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
            cp arch/x86/test/vnet-private-kernel32.def "$out/vnet-kernel32.def"
        else
            target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64
            sed 's/@[0-9][0-9]*$//' arch/x86/test/vnet-private-kernel32.def > "$out/vnet-kernel32.def"
        fi
        llvm-dlltool -m "$dll_machine" -k -d "$out/vnet-kernel32.def" -l "$out/vnet-kernel32.lib"
        clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
            -c arch/x86/test/vnet_private_pe.c -o "$out/vnet$bits.obj"
        lld-link /entry:mainCRTStartup /subsystem:console /nodefaultlib /machine:"$machine" /fixed \
            "/out:$out/vnet$bits.exe" "$out/vnet$bits.obj" "$out/vnet-kernel32.lib"
        tools/ositofs/ositofs-write3 "$work/ssh-test.img" "$out/vnet$bits.exe" --dest "/vnet$bits.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_IPHLP_DNS:-0} == 1 ]]; then
    for bits in 32 64; do
        if [[ $bits == 32 ]]; then
            target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
            cp arch/x86/test/iphlp-dns-kernel32.def "$out/dns-kernel32.def"
        else
            target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64
            sed 's/@[0-9][0-9]*$//' arch/x86/test/iphlp-dns-kernel32.def > "$out/dns-kernel32.def"
        fi
        llvm-dlltool -m "$dll_machine" -k -d "$out/dns-kernel32.def" -l "$out/dns-kernel32.lib"
        clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
            -c arch/x86/test/iphlp_dns_pe.c -o "$out/dns$bits.obj"
        lld-link /entry:mainCRTStartup /subsystem:console /nodefaultlib /machine:"$machine" /fixed \
            "/out:$out/dns$bits.exe" "$out/dns$bits.obj" "$out/dns-kernel32.lib"
        tools/ositofs/ositofs-write3 "$work/ssh-test.img" "$out/dns$bits.exe" --dest "/dns$bits.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_REMOTE_IO:-0} == 1 ]]; then
    for bits in 32 64; do
        if [[ $bits == 32 ]]; then
            target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
            cp arch/x86/test/remote-io-kernel32.def "$out/io-kernel32.def"
        else
            target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64
            sed 's/@[0-9][0-9]*$//' arch/x86/test/remote-io-kernel32.def > "$out/io-kernel32.def"
        fi
        llvm-dlltool -m "$dll_machine" -k -d "$out/io-kernel32.def" -l "$out/io-kernel32.lib"
        clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
            -c arch/x86/test/remote_io_pe.c -o "$out/io$bits.obj"
        lld-link /entry:mainCRTStartup /subsystem:console /nodefaultlib /machine:"$machine" /fixed \
            "/out:$out/io$bits.exe" "$out/io$bits.obj" "$out/io-kernel32.lib"
        tools/ositofs/ositofs-write3 "$work/ssh-test.img" "$out/io$bits.exe" --dest "/io$bits.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_REMOTE_MAINTENANCE:-0} == 1 ]]; then
    for bits in 32 64; do
        if [[ $bits == 32 ]]; then
            target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
            cp arch/x86/test/registry-concurrent-kernel32.def "$out/maint-kernel32.def"
        else
            target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64
            sed 's/@[0-9][0-9]*$//' arch/x86/test/registry-concurrent-kernel32.def > "$out/maint-kernel32.def"
        fi
        llvm-dlltool -m "$dll_machine" -k -d "$out/maint-kernel32.def" -l "$out/maint-kernel32.lib"
        clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
            -c arch/x86/test/remote_maintenance_pe.c -o "$out/maint$bits.obj"
        lld-link /entry:mainCRTStartup /subsystem:console /nodefaultlib /machine:"$machine" /fixed \
            "/out:$out/maint$bits.exe" "$out/maint$bits.obj" "$out/maint-kernel32.lib"
        tools/ositofs/ositofs-write3 "$work/ssh-test.img" "$out/maint$bits.exe" --dest "/maint$bits.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_REGISTRY_CONCURRENT:-0} == 1 ]]; then
    for bits in 32 64; do
        if [[ $bits == 32 ]]; then
            target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
            cp arch/x86/test/registry-concurrent-kernel32.def "$out/registry-kernel32.def"
        else
            target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64
            sed 's/@[0-9][0-9]*$//' arch/x86/test/registry-concurrent-kernel32.def > "$out/registry-kernel32.def"
        fi
        llvm-dlltool -m "$dll_machine" -k -d "$out/registry-kernel32.def" -l "$out/registry-kernel32.lib"
        clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
            -c arch/x86/test/registry_concurrent_pe.c -o "$out/registry$bits.obj"
        lld-link /entry:mainCRTStartup /subsystem:console /nodefaultlib /machine:"$machine" /fixed \
            "/out:$out/registry$bits.exe" "$out/registry$bits.obj" "$out/registry-kernel32.lib"
        tools/ositofs/ositofs-write3 "$work/ssh-test.img" "$out/registry$bits.exe" --dest "/registry$bits.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_REGISTRY_CACHE:-0} == 1 ]]; then
    bash arch/x86/scripts/build-registry-cache-test.sh "$out/regcache"
    for bits in 32 64; do
        write_boot_fixture "$out/regcache/regcache$bits.exe" "/regcache$bits.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_GDI_SURFACES:-0} == 1 ]]; then
    for suite in gdi-paint gdi-dib gdi-text user32-paint; do
        bash "arch/x86/scripts/build-$suite-test.sh" "$out/$suite"
        for bits in 32 64; do
            write_boot_fixture "$out/$suite/pe$bits/${suite//-/_}_pe$bits.exe" "/$suite$bits.exe"
        done
    done
fi
if [[ ${OSITO_SSH_TEST_LAYERED:-0} == 1 ]]; then
    bash arch/x86/scripts/build-layered-window-test.sh "$out"
    for bits in 32 64; do
        tools/ositofs/ositofs-write3 "$work/ssh-test.img" "$out/layered$bits.exe" --dest "/layered$bits.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_ACCESS_CHECK:-0} == 1 ]]; then
    bash arch/x86/scripts/build-access-check-test.sh "$out/accesscheck"
    for bits in 32 64; do
        write_boot_fixture "$out/accesscheck/accesscheck$bits.exe" "/accesscheck$bits.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_RESTRICTED_TOKEN:-0} == 1 ]]; then
    bash arch/x86/scripts/build-restricted-token-test.sh "$out/restricted"
    for bits in 32 64; do
        write_boot_fixture "$out/restricted/restricted$bits.exe" "/restricted$bits.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_TOKEN_POLICY:-0} == 1 ]]; then
    bash arch/x86/scripts/build-token-policy-test.sh "$out/tokenpolicy"
    for bits in 32 64; do
        write_boot_fixture "$out/tokenpolicy/tokenpolicy$bits.exe" "/tokenpolicy$bits.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_TOKEN_INTEGRITY:-0} == 1 ]]; then
    bash arch/x86/scripts/build-token-integrity-test.sh "$out/tokenintegrity"
    for bits in 32 64; do
        write_boot_fixture "$out/tokenintegrity/tokenintegrity$bits.exe" "/tokenintegrity$bits.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_TOKEN_PRIVILEGES:-0} == 1 ]]; then
    bash arch/x86/scripts/build-token-privileges-test.sh "$out/tokenprivileges"
    for bits in 32 64; do
        write_boot_fixture "$out/tokenprivileges/tokenprivileges$bits.exe" "/tokenprivileges$bits.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_MANDATORY_ACE:-0} == 1 ]]; then
    bash arch/x86/scripts/build-mandatory-ace-test.sh "$out/mandatoryace"
    for bits in 32 64; do
        write_boot_fixture "$out/mandatoryace/mandatoryace$bits.exe" "/mandatoryace$bits.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_OPEN_PROCESS:-0} == 1 ]]; then
    bash arch/x86/scripts/build-open-process-test.sh "$out/openprocess"
    for bits in 32 64; do
        write_boot_fixture "$out/openprocess/openprocess$bits.exe" "/openprocess$bits.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_NATIVE_TOKEN:-0} == 1 ]]; then
    bash arch/x86/scripts/build-native-token-test.sh "$out/nativetoken"
    for bits in 32 64; do
        write_boot_fixture "$out/nativetoken/nativetoken$bits.exe" "/nativetoken$bits.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_THREAD_INFO:-0} == 1 ]]; then
    bash arch/x86/scripts/build-thread-info-test.sh "$out/threadinfo"
    for bits in 32 64; do
        write_boot_fixture "$out/threadinfo/threadinfo$bits.exe" "/threadinfo$bits.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_JOB_IOCP:-0} == 1 ]]; then
    bash arch/x86/scripts/build-job-iocp-test.sh "$out/jobiocp"
    for bits in 32 64; do
        write_boot_fixture "$out/jobiocp/jobiocp$bits.exe" "/jobiocp$bits.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_OPEN_THREAD:-0} == 1 ]]; then
    bash arch/x86/scripts/build-open-thread-test.sh "$out/openthread"
    for bits in 32 64; do
        write_boot_fixture "$out/openthread/openthread$bits.exe" "/openthread$bits.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_REMOTE_VM:-0} == 1 ]]; then
    bash arch/x86/scripts/build-remote-vm-test.sh "$out/remotevm"
    for bits in 32 64; do
        write_boot_fixture "$out/remotevm/remotevm$bits.exe" "/remotevm$bits.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_SUSPENDED_IMAGE:-0} == 1 ]]; then
    bash arch/x86/scripts/build-suspended-image-test.sh "$out/suspended"
    for bits in 32 64; do
        write_boot_fixture "$out/suspended/suspended$bits.exe" "/suspended$bits.exe"
        write_boot_fixture "$out/suspended/suspendeddep$bits.dll" "/suspendeddep$bits.dll"
    done
fi
if [[ ${OSITO_SSH_TEST_SHIM_EXPORTS:-0} == 1 ]]; then
    bash arch/x86/scripts/build-shim-exports-test.sh "$out/shimexports"
    for bits in 32 64; do
        write_boot_fixture "$out/shimexports/shimexports$bits.exe" "/shimexports$bits.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_NT64_SERVICE:-0} == 1 ]]; then
    bash arch/x86/scripts/build-nt64-service-test.sh "$out/nt64-service"
    write_boot_fixture "$out/nt64-service/nt64-service.exe" /nt64-service.exe
    write_boot_fixture "$out/nt64-service/nt64-elf" /nt64-elf
fi
if [[ ${OSITO_SSH_TEST_PROCESS_TOKEN:-0} == 1 ]]; then
    bash arch/x86/scripts/build-process-token-test.sh "$out/processtoken"
    for bits in 32 64; do
        write_boot_fixture "$out/processtoken/processtoken$bits.exe" "/processtoken$bits.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_WINSOCK_STREAM:-0} == 1 ]]; then
    bash arch/x86/scripts/build-winsock-stream-test.sh "$out/stream"
    for bits in 32 64; do
        write_boot_fixture "$out/stream/stream$bits.exe" "/stream$bits.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_DISCONNECT:-0} == 1 ]]; then
    bash arch/x86/scripts/build-disconnect-test.sh "$out/disconnect"
    for bits in 32 64; do
        write_boot_fixture "$out/disconnect/disconnect$bits.exe" "/disconnect$bits.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_PROCESS_HANDLES:-0} == 1 ]]; then
    bash arch/x86/scripts/build-process-handles-test.sh "$out/prochandles"
    for bits in 32 64; do
        write_boot_fixture "$out/prochandles/prochandles$bits.exe" "/prochandles$bits.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_JOB_UI_SYSTEM:-0} == 1 ]]; then
    bash arch/x86/scripts/build-job-ui-system-test.sh "$out/jobuisystem"
    for bits in 32 64; do
        write_boot_fixture "$out/jobuisystem/jobuisystem$bits.exe" "/jobuisystem$bits.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_JOB_UI_HANDLES:-0} == 1 ]]; then
    bash arch/x86/scripts/build-job-ui-handles-test.sh "$out/jobuihandles"
    for bits in 32 64; do
        write_boot_fixture "$out/jobuihandles/jobuihandles$bits.exe" "/jobuihandles$bits.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_JOB_UI_ACCESS:-0} == 1 ]]; then
    bash arch/x86/scripts/build-job-ui-access-test.sh "$out/jobuiaccess"
    for bits in 32 64; do
        write_boot_fixture "$out/jobuiaccess/jobuiaccess$bits.exe" "/jobuiaccess$bits.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_JOB_UI_ATOMS:-0} == 1 ]]; then
    bash arch/x86/scripts/build-job-ui-atoms-test.sh "$out/jobuiatoms"
    for bits in 32 64; do
        write_boot_fixture "$out/jobuiatoms/jobuiatoms$bits.exe" "/jobuiatoms$bits.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_JOB_OBJECT:-0} == 1 ]]; then
    bash arch/x86/scripts/build-job-object-test.sh "$out/jobobject"
    for bits in 32 64; do
        write_boot_fixture "$out/jobobject/jobobject$bits.exe" "/jobobject$bits.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_USER_OBJECT:-0} == 1 ]]; then
    bash arch/x86/scripts/build-user-object-test.sh "$out/userobject"
    for bits in 32 64; do
        write_boot_fixture "$out/userobject/userobject$bits.exe" "/userobject$bits.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_USER32_CASE:-0} == 1 ]]; then
    for bits in 32 64; do
        if [[ $bits == 32 ]]; then
            target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
            cp arch/x86/test/wgl-backend-kernel32.def "$out/case-kernel32.def"
        else
            target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64
            sed 's/@[0-9][0-9]*$//' arch/x86/test/wgl-backend-kernel32.def > "$out/case-kernel32.def"
        fi
        llvm-dlltool -m "$dll_machine" -k -d "$out/case-kernel32.def" -l "$out/case-kernel32.lib"
        clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
            -c arch/x86/test/user32_case_pe.c -o "$out/case$bits.obj"
        lld-link /entry:mainCRTStartup /subsystem:console /nodefaultlib /machine:"$machine" /fixed \
            "/out:$out/case$bits.exe" "$out/case$bits.obj" "$out/case-kernel32.lib"
        tools/ositofs/ositofs-write3 "$work/ssh-test.img" "$out/case$bits.exe" --dest "/case$bits.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_DWRITE_NUMBER:-0} == 1 ]]; then
    for bits in 32 64; do
        if [[ $bits == 32 ]]; then
            target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
            cp arch/x86/test/wgl-backend-kernel32.def "$out/graphics-kernel32.def"
        else
            target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64
            sed 's/@[0-9][0-9]*$//' arch/x86/test/wgl-backend-kernel32.def > "$out/graphics-kernel32.def"
        fi
        llvm-dlltool -m "$dll_machine" -k -d "$out/graphics-kernel32.def" -l "$out/graphics-kernel32.lib"
        clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
            -c arch/x86/test/d3d9_backend_pe.c -o "$out/d3d9$bits.obj"
        lld-link /entry:mainCRTStartup /subsystem:console /nodefaultlib /machine:"$machine" /fixed \
            "/out:$out/d3d9$bits.exe" "$out/d3d9$bits.obj" "$out/graphics-kernel32.lib"
        tools/ositofs/ositofs-write3 "$work/ssh-test.img" "$out/d3d9$bits.exe" --dest "/d3d9$bits.exe"
    done
    clang --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin -fno-stack-protector \
        -c arch/x86/test/dwrite_number_pe64.c -o "$out/dwrite-number.obj"
    lld-link /entry:mainCRTStartup /subsystem:console /nodefaultlib /machine:x64 /fixed \
        "/out:$out/dwrite-number.exe" "$out/dwrite-number.obj" "$out/graphics-kernel32.lib"
    tools/ositofs/ositofs-write3 "$work/ssh-test.img" "$out/dwrite-number.exe" --dest /dwrite-number.exe
fi
if [[ ${OSITO_SSH_TEST_CONSOLE_ATTACH:-0} == 1 ]]; then
    for bits in 32 64; do
        if [[ $bits == 32 ]]; then
            target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
            cp arch/x86/test/console-attach-kernel32.def "$out/console-kernel32.def"
        else
            target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64
            sed 's/@[0-9][0-9]*$//' arch/x86/test/console-attach-kernel32.def > "$out/console-kernel32.def"
        fi
        llvm-dlltool -m "$dll_machine" -k -d "$out/console-kernel32.def" -l "$out/console-kernel32.lib"
        name="conattach$bits"
        clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
            -c arch/x86/test/console_attach_pe.c -o "$out/$name.obj"
        lld-link /entry:mainCRTStartup /subsystem:console /nodefaultlib /machine:"$machine" /fixed \
            "/out:$out/$name.exe" "$out/$name.obj" "$out/console-kernel32.lib"
        tools/ositofs/ositofs-write3 "$work/ssh-test.img" "$out/$name.exe" --dest "/$name.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_SEH_DIAGNOSTIC:-0} == 1 ]]; then
    bash arch/x86/scripts/build-seh-diagnostic-test.sh "$out/seh"
    write_boot_fixture "$out/seh/seh-launch.exe" /seh-launch.exe
    for bits in 32 64; do
        for case_id in 0 1 2 3 4 5 6 7; do
            write_boot_fixture "$out/seh/pe$bits/seh$bits-$case_id.exe" "/seh$bits-$case_id.exe"
        done
    done
fi
if [[ ${OSITO_SSH_TEST_FAULT_READ:-0} == 1 ]]; then
    for bits in 32 64; do
        if [[ $bits == 32 ]]; then
            target=i686-pc-windows-msvc; machine=x86; dll_machine=i386
            cp arch/x86/test/fault-read-kernel32.def "$out/fault-kernel32.def"
        else
            target=x86_64-pc-windows-msvc; machine=x64; dll_machine=i386:x86-64
            sed 's/@[0-9][0-9]*$//' arch/x86/test/fault-read-kernel32.def > "$out/fault-kernel32.def"
        fi
        llvm-dlltool -m "$dll_machine" -k -d "$out/fault-kernel32.def" -l "$out/fault-kernel32.lib"
        for boundary in 0 1; do
            name="fault$bits-$boundary"
            clang --target="$target" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
                -DFAULT_READ_BOUNDARY="$boundary" -c arch/x86/test/fault_read_pe.c -o "$out/$name.obj"
            lld-link /entry:mainCRTStartup /subsystem:console /nodefaultlib /machine:"$machine" /fixed \
                "/out:$out/$name.exe" "$out/$name.obj" "$out/fault-kernel32.lib"
            tools/ositofs/ositofs-write3 "$work/ssh-test.img" "$out/$name.exe" --dest "/$name.exe"
        done
    done
    clang --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin -fno-stack-protector \
        -DFAULT_READ_LAUNCHER=1 -c arch/x86/test/fault_read_pe.c -o "$out/fault-launch.obj"
    lld-link /entry:mainCRTStartup /subsystem:console /nodefaultlib /machine:x64 /fixed \
        "/out:$out/fault-launch.exe" "$out/fault-launch.obj" "$out/fault-kernel32.lib"
    tools/ositofs/ositofs-write3 "$work/ssh-test.img" "$out/fault-launch.exe" --dest /fault-launch.exe
fi
if [[ ${OSITO_SSH_TEST_CPU_CYCLES:-0} == 1 ]]; then
    bash arch/x86/scripts/build-cpu-cycles-test.sh "$out/cpu-cycles"
    for bits in 32 64; do
        for name in cycles cycleschild; do
            tools/ositofs/ositofs-write3 "$work/ssh-test.img" \
                "$out/cpu-cycles/pe$bits/$name$bits.exe" --dest "/$name$bits.exe"
        done
    done
fi
if [[ ${OSITO_SSH_TEST_WGL:-0} == 1 ]]; then
    bash arch/x86/scripts/build-wgl-backend-test.sh "$out/wgl"
    for bits in 32 64; do
        tools/ositofs/ositofs-write3 "$work/ssh-test.img" \
            "$out/wgl/pe$bits/wgl_backend_pe$bits.exe" --dest "/wgl$bits.exe"
    done
    if [[ -n ${OSITO_SSH_TEST_WGL_HELPER_DIR:-} ]]; then
        for asset in libGL-osito.so gldriverquery64.exe; do
            tools/ositofs/ositofs-write3 "$work/ssh-test.img" \
                "$OSITO_SSH_TEST_WGL_HELPER_DIR/$asset" --dest "/$asset"
        done
    fi
fi
if [[ ${OSITO_SSH_TEST_FILE_CACHE:-0} == 1 ]]; then
    bash arch/x86/scripts/build-file-cache-test.sh "$out/file-cache"
    for bits in 32 64; do
        write_boot_fixture "$out/file-cache/filecache$bits.exe" "/filecache$bits.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_USB_READ:-0} == 1 ]]; then
    test "${OSITO_SSH_TEST_STORAGE:-nvme}" = usb
    bash arch/x86/scripts/build-usb-read-test.sh "$out/usb-read"
    for bits in 32 64; do
        write_boot_fixture "$out/usb-read/usbread$bits.exe" "/usbread$bits.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_USB_STACK:-0} == 1 ]]; then
    test "${OSITO_SSH_TEST_STORAGE:-nvme}" = usb
    llvm-dlltool -m i386:x86-64 -d arch/x86/test/usb-stack-kernel32.def -l "$out/kernel32.lib"
    clang --target=x86_64-w64-windows-gnu -O2 -ffreestanding -fno-stack-protector \
        -c arch/x86/test/usb_stack_pe64.c -o "$out/usb-stack64.obj"
    lld-link /entry:mainCRTStartup /subsystem:console /nodefaultlib /machine:x64 \
        "/out:$out/usbdma64.exe" "$out/usb-stack64.obj" "$out/kernel32.lib"
    tools/ositofs/ositofs-write3 "$work/ssh-test.img" "$out/usbdma64.exe" --dest /usbdma64.exe
fi
if [[ ${OSITO_SSH_TEST_THREAD_OWNER:-0} == 1 ]]; then
    llvm-dlltool -m i386:x86-64 -d arch/x86/test/thread-owner-kernel32.def -l "$out/kernel32.lib"
    for name in ownparent ownchild; do
        flags=()
        if [[ $name == ownchild ]]; then flags+=(-DOWNER_CHILD=1); fi
        clang --target=x86_64-w64-windows-gnu -O2 -ffreestanding -fno-stack-protector \
            "${flags[@]}" -c arch/x86/test/thread_owner_pe64.c -o "$out/$name.obj"
        lld-link /entry:mainCRTStartup /subsystem:console /nodefaultlib /machine:x64 \
            "/out:$out/$name.exe" "$out/$name.obj" "$out/kernel32.lib"
        tools/ositofs/ositofs-write3 "$work/ssh-test.img" "$out/$name.exe" --dest "/$name.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_TLS_REAP:-0} == 1 ]]; then
    llvm-dlltool -m i386 -k -d arch/x86/test/pf-imports-kernel32.def -l "$out/pf-kernel32.lib"
    llvm-dlltool -m i386 -d arch/x86/test/pf-imports-msvcr120.def -l "$out/pf-msvcr120.lib"
    clang --target=i686-pc-windows-msvc -O2 -ffreestanding -fno-builtin -fno-stack-protector \
        -c arch/x86/test/pf_imports_pe32.c -o "$out/pfimports32.obj"
    lld-link /entry:mainCRTStartup /subsystem:console /nodefaultlib /machine:x86 /fixed \
        "/out:$out/pfimports32.exe" "$out/pfimports32.obj" "$out/pf-kernel32.lib" "$out/pf-msvcr120.lib"
    tools/ositofs/ositofs-write3 "$work/ssh-test.img" "$out/pfimports32.exe" --dest /pfimports32.exe
    llvm-dlltool -m i386:x86-64 -d arch/x86/test/tls-reap-kernel32.def -l "$out/kernel32.lib"
    for name in tlsparent tlschild; do
        flags=()
        if [[ $name == tlschild ]]; then flags+=(-DTLS_REAP_CHILD=1); fi
        clang --target=x86_64-w64-windows-gnu -O2 -ffreestanding -fno-stack-protector \
            "${flags[@]}" -c arch/x86/test/tls_reap_pe64.c -o "$out/$name.obj"
        lld-link /entry:mainCRTStartup /subsystem:console /nodefaultlib /machine:x64 \
            "/out:$out/$name.exe" "$out/$name.obj" "$out/kernel32.lib"
        tools/ositofs/ositofs-write3 "$work/ssh-test.img" "$out/$name.exe" --dest "/$name.exe"
    done
fi
if [[ ${OSITO_SSH_TEST_LOCKUP:-0} == 1 ]]; then
    llvm-dlltool -m i386:x86-64 -d arch/x86/test/lockup-kernel32.def -l "$out/kernel32.lib"
    clang --target=x86_64-w64-windows-gnu -O2 -ffreestanding -fno-stack-protector \
        -c arch/x86/test/lockup_pe64.c -o "$out/lockup64.obj"
    lld-link /entry:mainCRTStartup /subsystem:console /nodefaultlib /machine:x64 \
        "/out:$out/lockup64.exe" "$out/lockup64.obj" "$out/kernel32.lib"
    tools/ositofs/ositofs-write3 "$work/ssh-test.img" "$out/lockup64.exe" --dest /lockup64.exe
fi
if [[ ${OSITO_SSH_TEST_FILEGUARD:-0} == 1 ]]; then
    llvm-dlltool -m i386:x86-64 -d arch/x86/test/fileguard-kernel32.def -l "$out/kernel32.lib"
    for name in fgparent fgchild; do
        flags=()
        if [[ $name == fgchild ]]; then flags+=(-DFILEGUARD_CHILD=1); fi
        clang --target=x86_64-w64-windows-gnu -O2 -ffreestanding -fno-stack-protector \
            "${flags[@]}" -c arch/x86/test/fileguard_pe64.c -o "$out/$name.obj"
        lld-link /entry:mainCRTStartup /subsystem:console /nodefaultlib /machine:x64 \
            "/out:$out/$name.exe" "$out/$name.obj" "$out/kernel32.lib"
        tools/ositofs/ositofs-write3 "$work/ssh-test.img" "$out/$name.exe" --dest "/$name.exe"
    done
    bash arch/x86/scripts/build-file-lock-contract-test.sh "$out/pe32"
    tools/ositofs/ositofs-write3 "$work/ssh-test.img" "$out/pe32/file_lock_contract_pe32.exe" --dest /flock32.exe
fi
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$work/vars.fd"
python3 arch/x86/scripts/test-ssh-qemu.py "$out"
