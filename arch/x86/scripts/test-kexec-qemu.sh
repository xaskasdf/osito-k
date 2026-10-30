#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
if [[ -z ${OSITO_TEST_DISK_DIR:-} ]]; then
    exec bash arch/x86/scripts/qemu-test-disks.sh run -- bash "$0" "$@"
fi
out=${1:?output directory required}
mode=${2:-fixed}
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
tools/ositofs/mkfs.ositofs3 "$work/data.img" --label KexecTest --inodes 4096
touch "$work/noautoload"
tools/ositofs/ositofs-write3 "$work/data.img" "$work/noautoload" --dest /.noautoload
tools/ositofs/ositofs-write3 "$work/data.img" "$out/kernel.elf" --dest /next.elf
tools/ositofs/ositofs-write3 "$work/data.img" arch/x86/kernel/kexec.h --dest /bad.elf
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$work/vars.fd"
python3 arch/x86/scripts/test-kexec-qemu.py "$out" "$mode"
