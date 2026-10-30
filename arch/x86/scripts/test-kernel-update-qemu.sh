#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
if [[ -z ${OSITO_TEST_DISK_DIR:-} ]]; then
    exec bash arch/x86/scripts/qemu-test-disks.sh run -- bash "$0" "$@"
fi
out=${1:?output directory required}
test ! -e "$out"
mkdir -p "$out"
out=$(realpath "$out")
work=$OSITO_TEST_DISK_DIR
cp arch/x86/build/kernel.elf arch/x86/build/boot.efi "$out/"
truncate -s 128M "$work/esp.img"
mformat -i "$work/esp.img" -F ::
mmd -i "$work/esp.img" ::/EFI ::/EFI/BOOT
mcopy -i "$work/esp.img" "$out/boot.efi" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$work/esp.img" "$out/kernel.elf" ::/EFI/BOOT/kernel.elf
truncate -s 128M "$work/data.img"
tools/ositofs/mkfs.ositofs3 "$work/data.img" --label KupdateTest --inodes 4096
touch "$work/noautoload"
tools/ositofs/ositofs-write3 "$work/data.img" "$work/noautoload" --dest /.noautoload
cp "$out/kernel.elf" "$out/next.elf"
printf 'kupdate persistent test\n' >> "$out/next.elf"
tools/ositofs/ositofs-write3 "$work/data.img" "$out/next.elf" --dest /next.elf
tools/ositofs/ositofs-write3 "$work/data.img" arch/x86/kernel/kernel_update.h --dest /bad.elf
truncate -s 260M "$work/usb.img"
sgdisk --clear --new=1:2048:264191 --typecode=1:ef00 --change-name=1:ESP \
    --new=2:264192:526335 --typecode=2:8300 --change-name=2:OsitoFS "$work/usb.img"
dd if="$work/esp.img" of="$work/usb.img" bs=1M seek=1 conv=notrunc status=none
dd if="$work/data.img" of="$work/usb.img" bs=1M seek=129 conv=notrunc status=none
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$work/vars.fd"
python3 arch/x86/scripts/test-kernel-update-qemu.py "$out"
dd if="$work/usb.img" of="$work/result-esp.img" bs=1M skip=1 count=128 status=none
mcopy -i "$work/result-esp.img" ::/EFI/BOOT/kernel.elf "$out/installed.elf"
cmp "$out/next.elf" "$out/installed.elf"
fsck.fat -n "$work/result-esp.img" | tee "$out/fsck-fat.log"
echo 'PASS: cold boot from updated USB; EFI payload exact; FAT clean'
