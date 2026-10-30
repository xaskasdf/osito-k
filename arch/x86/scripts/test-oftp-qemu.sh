#!/bin/bash
# Isolated OFTP regression: never attaches a persistent test disk or USB device.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$repo"
if [ -z "${OSITO_TEST_DISK_DIR:-}" ]; then
    exec bash arch/x86/scripts/qemu-test-disks.sh run -- bash "$0" "$@"
fi
out=${1:-arch/x86/build/oftp-qemu}
firmware=${2:-}
test ! -e "$out"
mkdir -p "$out"
out=$(realpath "$out")
cp --parents "$0" arch/x86/scripts/test-oftp-qemu.py arch/x86/kernel/shell.c \
    arch/x86/kernel/oftp_rx.h "$out/"
cp arch/x86/build/kernel.elf "$out/kernel.elf"
cp arch/x86/build/boot.efi "$out/boot.efi"
esp="$OSITO_TEST_DISK_DIR/esp.img"
disk="$OSITO_TEST_DISK_DIR/net-test.img"
test ! -e "$esp"
test ! -e "$disk"
truncate -s 128M "$esp"
mformat -i "$esp" -F ::
mmd -i "$esp" ::/EFI ::/EFI/BOOT
mcopy -i "$esp" "$out/boot.efi" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$esp" "$out/kernel.elf" ::/EFI/BOOT/kernel.elf
truncate -s 128M "$disk"
tools/ositofs/mkfs.ositofs3 "$disk" --label NetTest --inodes 4096
touch "$OSITO_TEST_DISK_DIR/noautoload"
tools/ositofs/ositofs-write3 "$disk" "$OSITO_TEST_DISK_DIR/noautoload" --dest /.noautoload
mkdir "$OSITO_TEST_DISK_DIR/diag"
tools/ositofs/ositofs-write3 "$disk" "$OSITO_TEST_DISK_DIR/diag" --dest /diag -r
if [[ -n $firmware ]]; then
    cp "$firmware" "$out/firmware.ucode"
    sha256sum "$out/firmware.ucode" > "$out/firmware.sha256"
    tools/ositofs/ositofs-write3 "$disk" "$out/firmware.ucode" --dest /iwlwifi-cc-a0-77.ucode
    tools/ositofs/ositofs-write3 "$disk" "$out/boot.efi" --dest /invalid-fw.bin
fi
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$OSITO_TEST_DISK_DIR/vars.fd"
python3 arch/x86/scripts/test-oftp-qemu.py "$out"
