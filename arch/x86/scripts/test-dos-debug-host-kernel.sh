#!/bin/bash
# Boot a candidate Linux kernel without changing the running WSL installation.
set -euo pipefail
if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
    echo "Usage: $0 BZIMAGE FRESH_EVIDENCE_DIR [PAYLOAD_EVIDENCE_DIR]" >&2
    exit 2
fi
repo=$(cd "$(dirname "$0")/../../.." && pwd)
kernel=$(realpath -e "$1")
run=$2
payload=
if [ "$#" = 3 ]; then
    payload=$(realpath -e "$3")
    (cd "$payload/sources"; sha256sum -c "$payload/sources.sha256")
    sha256sum -c "$payload/binaries.sha256"
fi
mkdir "$run"
run=$(realpath -e "$run")
cd "$repo"
if [ -n "$payload" ]; then
    cp -a "$payload/root" "$payload/sources" "$run/"
    cp "$payload/sources.sha256" "$payload/initramfs.cpio.gz" "$run/"
    printf '%s\n' "$payload" > "$run/payload-source.txt"
    cmp "$payload/initramfs.cpio.gz" "$run/initramfs.cpio.gz"
else
    mkdir "$run/root" "$run/sources"
    cc=${CC:-gcc}
    sources=(arch/x86/test/dos_debug_native_host.c arch/x86/test/dos_debug_native_guest.S
             arch/x86/test/dos_debug_native_init.c arch/x86/scripts/test-dos-debug-host-kernel.sh)
    cp --parents "${sources[@]}" "$run/sources/"
    sha256sum "${sources[@]}" > "$run/sources.sha256"
    "$cc" --version > "$run/compiler.txt"
    "$cc" -std=c11 -static -O2 -Wall -Wextra -Werror \
        arch/x86/test/dos_debug_native_host.c -o "$run/root/debug-host"
    "$cc" -std=c11 -static -O2 -Wall -Wextra -Werror \
        arch/x86/test/dos_debug_native_init.c -o "$run/root/init"
    "$cc" -c arch/x86/test/dos_debug_native_guest.S -o "$run/guest.o"
    objcopy -O binary -j .text "$run/guest.o" "$run/root/guest.bin"
    (cd "$run/root"; find . -print0 | sort -z | cpio --null -o --format=newc 2> "$run/cpio.log") |
        gzip -n > "$run/initramfs.cpio.gz"
fi
cp "$repo/arch/x86/scripts/test-dos-debug-host-kernel.sh" "$run/runner.sh"
cp "$kernel" "$run/kernel.bzImage"
qemu=${QEMU_BIN:-qemu-system-x86_64}
"$qemu" --version > "$run/qemu-version.txt"
uname -a > "$run/outer-host.txt"
args=(-name osito-kvm-host-test -machine q35,accel=kvm -cpu host -m 1G -smp 2
      -nodefaults -display none -no-reboot -serial "file:$run/serial.log" -monitor none
      -pidfile "$run/qemu.pid" -qmp "unix:$run/qmp.sock,server=on,wait=off"
      -kernel "$run/kernel.bzImage" -initrd "$run/initramfs.cpio.gz"
      -append "console=ttyS0,115200 rdinit=/init panic=-1 ${OSITO_KVM_TEST_KERNEL_ARGS:-}")
printf '%q ' "$qemu" "${args[@]}" > "$run/qemu-command.txt"
printf '\n' >> "$run/qemu-command.txt"
sha256sum "$run/kernel.bzImage" "$run/initramfs.cpio.gz" "$run/root/"* "$run/runner.sh" > "$run/binaries.sha256"
rc=0
timeout --signal=TERM --kill-after=5s 240s "$qemu" "${args[@]}" > "$run/qemu.log" 2>&1 || rc=$?
printf '%s\n' "$rc" > "$run/qemu.exit"
if [ "$rc" != 0 ]; then
    echo "QEMU did not complete normally (status $rc): $run" >&2
    exit "$rc"
fi
probe=$(sed -n 's/^\[KVM-HOST-BOOT\] probe-exit=\([0-9]*\)\r\{0,1\}$/\1/p' "$run/serial.log")
[[ "$probe" =~ ^[0-9]+$ ]] && [ "$probe" -le 255 ] || {
    echo "No unique completed probe status: $run" >&2
    exit 125
}
printf '%s\n' "$probe" > "$run/probe.exit"
grep -e 'LINUX USER ICEBP' -e 'KVM DEBUG CONTRACT' -e 'KVM-HOST-BOOT' "$run/serial.log"
echo "Evidence: $run"
exit "$probe"
