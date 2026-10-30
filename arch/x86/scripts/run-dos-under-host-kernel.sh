#!/bin/bash
# Real Osito-K/DOS tests inside a disposable Linux/KVM host.
set -euo pipefail
# Parse the complete launch before waiting, so live source edits cannot alter it.
{
if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
    echo "Usage: $0 LINUX_BZIMAGE FRESH_EVIDENCE_DIR [PAYLOAD_EVIDENCE_DIR]" >&2
    exit 2
fi
script_dir=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$script_dir/../../.." && pwd)
if [ -z "${OSITO_TEST_DISK_DIR:-}" ]; then
    exec bash "$script_dir/qemu-test-disks.sh" run -- bash "$script_dir/run-dos-under-host-kernel.sh" "$@"
fi
kernel=$(realpath -e "$1")
mkdir "$2"
run=$(realpath -e "$2")
cd "$repo"
mkdir "$run/output" "$run/sources"
cp "$script_dir/run-dos-under-host-kernel.sh" "$run/runner.sh"
root=$OSITO_TEST_DISK_DIR/root
if [ "$#" = 3 ]; then
    payload=$(realpath -e "$3")
    (cd "$payload"; sha256sum -c payload.sha256)
    cp "$payload/initramfs.cpio.gz" "$run/"
    cp -a "$payload/sources/." "$run/sources/"
    cp "$payload/kernel.elf" "$run/kernel.elf"
    for metadata in root.sha256 dependencies.txt inner-qemu-version.txt; do
        cp "$payload/$metadata" "$run/$metadata"
    done
    if [ -f "$payload/sources.sha256" ]; then
        (cd "$run/sources"; sha256sum -c "$payload/sources.sha256")
    fi
    printf '%s\n' "$payload" > "$run/payload-source.txt"
else
    cp --parents arch/x86/test/dos_nested_host_init.c arch/x86/scripts/run-dos4gw-test.sh \
        arch/x86/test/dos_nested_serial_host.c arch/x86/scripts/qemu-test.sh \
        arch/x86/scripts/qemu-test-disks.sh "$run/sources/"
    cp arch/x86/build/dpmi-jit-checks.py arch/x86/build/doom-exit-smoke.py "$run/sources/"
    /usr/bin/qemu-system-x86_64 --version > "$run/inner-qemu-version.txt"
    QEMU_PREPARE_ONLY=1 bash "$script_dir/run-dos4gw-test.sh" "$run" > "$run/prepare.log" 2>&1
    mkdir -p "$root/usr/bin" "$root/usr/share" "$root/usr/lib/x86_64-linux-gnu/qemu" "$root/var/tmp"
    cp -L /usr/bin/qemu-system-x86_64 "$root/usr/bin/"
    modules=(/usr/lib/x86_64-linux-gnu/qemu/hw-display-virtio-vga.so
             /usr/lib/x86_64-linux-gnu/qemu/hw-display-virtio-gpu-pci.so
             /usr/lib/x86_64-linux-gnu/qemu/hw-display-virtio-gpu.so)
    cp "${modules[@]}" "$root/usr/lib/x86_64-linux-gnu/qemu/"
    ldd /usr/bin/qemu-system-x86_64 "${modules[@]}" > "$run/dependencies.txt"
    ! grep -q 'not found' "$run/dependencies.txt"
    while IFS= read -r library; do cp -L --parents "$library" "$root/"; done < <(
        awk '/=> \// {print $3} /^[[:space:]]*\/[^ ]+ [(]/ {print $1}' "$run/dependencies.txt" | sort -u)
    cp -aL /usr/share/qemu "$root/usr/share/"
    cp /usr/share/seabios/vgabios-virtio.bin "$root/usr/share/qemu/"
    cp /usr/share/ovmf/OVMF.fd "$root/OVMF.fd"
    cp --sparse=always "$OSITO_TEST_DISK_DIR/disk.img" "$root/disk.img"
    cp --sparse=always "$QEMU_ESP_IMG" "$root/esp.img"
    gcc -std=c11 -static -O2 -Wall -Wextra -Werror arch/x86/test/dos_nested_host_init.c -o "$root/init"
    (cd "$root"; find . -type f -print0 | sort -z | xargs -0 sha256sum) > "$run/root.sha256"
    (cd "$root"; find . -print0 | sort -z | cpio --null -o --format=newc 2> "$run/cpio.log") |
        gzip -n > "$run/initramfs.cpio.gz"
fi
(cd "$run/sources"; find . -type f -print0 | sort -z | xargs -0 sha256sum) > "$run/sources.sha256"
(cd "$run"; sha256sum initramfs.cpio.gz kernel.elf) > "$run/payload.sha256"
cp "$kernel" "$run/linux.bzImage"
qemu=${OSITO_OUTER_QEMU:-/root/qemu-venus-build/qemu-system-x86_64}
"$qemu" --version > "$run/outer-qemu-version.txt"
uname -a > "$run/outer-host.txt"
transport=$(mktemp -d /tmp/ositoknested.XXXXXXXX)
cleanup() {
    [ "$(realpath -e "$transport")" = "$transport" ] && [[ "$transport" == /tmp/ositoknested.* ]] || return 1
    rm -rf --one-file-system -- "$transport"
}
trap cleanup EXIT
printf '%s\n' "$transport" > "$run/transport.txt"
net=user,id=transport,restrict=on
for spec in serial:43290 monitor:43291 qmp:43292; do
    name=${spec%:*}
    port=${spec#*:}
    ln -s "$transport/$name.sock" "$run/$name.sock"
    net+=",hostfwd=unix:$transport/$name.sock-10.0.2.15:$port"
done
ln -s "$transport/outer.qmp" "$run/host-qmp.sock"
ln -s output/serial.log "$run/serial.log"
args=(-name osito-nested-linux -machine q35,accel=kvm -cpu host -m 10G -smp 6
      -nodefaults -display none -no-reboot -serial "file:$run/linux-serial.log"
      -monitor none -qmp "unix:$transport/outer.qmp,server=on,wait=off"
      -pidfile "$run/outer.pid" -device virtio-net-pci,netdev=transport -netdev "$net"
      -fsdev "local,id=evidence,path=$run/output,security_model=none"
      -device virtio-9p-pci,fsdev=evidence,mount_tag=evidence
      -kernel "$run/linux.bzImage" -initrd "$run/initramfs.cpio.gz"
      -append "console=ttyS0,115200 rdinit=/init panic=-1 ${OSITO_KVM_TEST_KERNEL_ARGS:-}")
printf '%q ' "$qemu" "${args[@]}" > "$run/outer-command.txt"
printf '\n' >> "$run/outer-command.txt"
sha256sum "$run/linux.bzImage" > "$run/linux.sha256"
rc=0
timeout --signal=TERM --kill-after=10s "${OSITO_NESTED_TIMEOUT:-7200}s" \
    "$qemu" "${args[@]}" > "$run/outer-qemu.log" 2>&1 || rc=$?
printf '%s\n' "$rc" > "$run/outer.exit"
[ "$rc" = 0 ] || exit "$rc"
[[ -f "$run/output/inner.exit" ]] || exit 125
mapfile -t statuses < "$run/output/inner.exit"
[ "${#statuses[@]}" = 1 ] || exit 125
status=${statuses[0]}
[[ "$status" =~ ^(0|[1-9][0-9]{0,2})$ ]] && [ "$status" -le 255 ] || exit 125
exit "$status"
}
