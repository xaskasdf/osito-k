#!/bin/bash
# Scratch HVF runner with a SERIAL SOCKET (drive the shell from drive.py).
# NVMe = nvme.img (gcc sysroot installed), -m 2048M.
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
X86_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$X86_DIR/build"
EFI_BIN="$BUILD_DIR/boot.efi"
KERN_BIN="$BUILD_DIR/kernel.elf"
ESP_IMG="$BUILD_DIR/esp.img"
SERIAL_LOG="$BUILD_DIR/serial.log"
SOCK="/tmp/osito-serial.sock"

# ── OVMF firmware ────────────────────────────────────────────
OVMF=""; OVMF_PFLASH=0; OVMF_VARS=""
for f in /usr/share/qemu/OVMF.fd /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF_CODE.fd; do
    [ -f "$f" ] && OVMF="$f" && break
done
if [ -z "$OVMF" ]; then
    for d in /usr/local/Cellar/qemu/*/share/qemu /opt/homebrew/Cellar/qemu/*/share/qemu \
             /opt/homebrew/share/qemu /usr/local/share/qemu; do
        if [ -f "$d/edk2-x86_64-code.fd" ]; then
            OVMF="$d/edk2-x86_64-code.fd"
            for varfile in "$d/edk2-i386-vars.fd" "$d/edk2-x86_64-vars.fd"; do
                [ -f "$varfile" ] && OVMF_VARS="$varfile" && break
            done
            OVMF_PFLASH=1; break
        fi
    done
fi

# ── ESP image (fresh kernel.elf) ─────────────────────────────
dd if=/dev/zero of="$ESP_IMG" bs=1M count=64 status=none
mformat -i "$ESP_IMG" -F ::
mmd -i "$ESP_IMG" ::/EFI
mmd -i "$ESP_IMG" ::/EFI/BOOT
mcopy -i "$ESP_IMG" "$EFI_BIN" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$ESP_IMG" "$KERN_BIN" ::/EFI/BOOT/kernel.elf
printf 'FS0:\\EFI\\BOOT\\BOOTX64.EFI\r\n' > /tmp/startup.nsh
mcopy -i "$ESP_IMG" /tmp/startup.nsh ::/startup.nsh

if [ "$OVMF_PFLASH" = "1" ]; then
    VARS_TMP="$BUILD_DIR/ovmf_vars.fd"; cp "$OVMF_VARS" "$VARS_TMP"
    BIOS_ARGS="-drive if=pflash,format=raw,readonly=on,file=$OVMF -drive if=pflash,format=raw,file=$VARS_TMP"
else
    BIOS_ARGS="-bios $OVMF"
fi

rm -f "$SERIAL_LOG" "$SOCK"
NVME_IMG="$BUILD_DIR/nvme.img"

qemu-system-x86_64 -accel hvf -cpu host \
    $BIOS_ARGS \
    -drive file="$ESP_IMG",format=raw,if=ide \
    -drive file="$NVME_IMG",format=raw,if=none,id=nvme0,cache=writeback,aio=threads \
    -device nvme,serial=deadbeef,drive=nvme0 \
    -m 2048M -machine q35 -smp 4 \
    -device virtio-net-pci,netdev=net0,disable-legacy=on,disable-modern=off \
    -netdev user,id=net0,hostfwd=udp::7778-:7777,hostfwd=tcp::50052-:50052 \
    -device qemu-xhci,id=usb -device usb-kbd,bus=usb.0 -device usb-mouse,bus=usb.0 \
    -display none \
    -chardev socket,id=s0,path="$SOCK",server=on,wait=off,logfile="$SERIAL_LOG" \
    -serial chardev:s0 \
    -no-reboot
