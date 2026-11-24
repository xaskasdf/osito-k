#!/bin/bash
#
# OsitoK x86-64 — QEMU Test Script (AGENT VERSION)
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
X86_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$X86_DIR/build"
EFI_BIN="$BUILD_DIR/boot.efi"
KERN_BIN="$BUILD_DIR/kernel.elf"
ESP_IMG="$BUILD_DIR/esp.img"

# OVMF firmware paths
for f in /usr/share/qemu/OVMF.fd \
         /usr/share/ovmf/OVMF.fd \
         /usr/share/OVMF/OVMF_CODE.fd; do
    [ -f "$f" ] && OVMF="$f" && break
done

if [ -z "$OVMF" ]; then
    QEMU_SHARE="$(qemu-system-x86_64 --version 2>/dev/null | head -1 | sed 's/.*version //')"
    for d in /usr/local/Cellar/qemu/*/share/qemu \
             /opt/homebrew/Cellar/qemu/*/share/qemu \
             /opt/homebrew/share/qemu \
             /usr/local/share/qemu; do
        if [ -f "$d/edk2-x86_64-code.fd" ]; then
            OVMF="$d/edk2-x86_64-code.fd"
            for varfile in "$d/edk2-i386-vars.fd" "$d/edk2-x86_64-vars.fd"; do
                [ -f "$varfile" ] && OVMF_VARS="$varfile" && break
            done
            OVMF_PFLASH=1
            break
        fi
    done
fi

# ── Create ESP image ─────────────────────────────────────────
dd if=/dev/zero of="$ESP_IMG" bs=1M count=64 status=none
mformat -i "$ESP_IMG" -F ::
mmd -i "$ESP_IMG" ::/EFI
mmd -i "$ESP_IMG" ::/EFI/BOOT
mcopy -i "$ESP_IMG" "$EFI_BIN" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$ESP_IMG" "$KERN_BIN" ::/EFI/BOOT/kernel.elf
printf 'FS0:\\EFI\\BOOT\\BOOTX64.EFI\r\n' > /tmp/startup.nsh
mcopy -i "$ESP_IMG" /tmp/startup.nsh ::/startup.nsh

# ── Launch QEMU ──────────────────────────────────────────────
if [ "${OVMF_PFLASH:-0}" = "1" ]; then
    VARS_TMP="$BUILD_DIR/ovmf_vars.fd"
    cp "$OVMF_VARS" "$VARS_TMP"
    BIOS_ARGS="-drive if=pflash,format=raw,readonly=on,file=$OVMF \
               -drive if=pflash,format=raw,file=$VARS_TMP"
else
    BIOS_ARGS="-bios $OVMF"
fi

SERIAL_LOG="$BUILD_DIR/serial.log"
rm -f "$SERIAL_LOG"

NVME_IMG="$BUILD_DIR/nvme.img"
NVME_ARGS=""
if [ -f "$NVME_IMG" ]; then
    NVME_ARGS="-drive file=$NVME_IMG,format=raw,if=none,id=nvme0,cache=none -device nvme,serial=deadbeef,drive=nvme0"
fi

qemu-system-x86_64 \
    $BIOS_ARGS \
    -drive file="$ESP_IMG",format=raw,if=ide \
    $NVME_ARGS \
    -m 512M \
    -machine q35 \
    -smp 4 \
    -device e1000e,netdev=net0 \
    -netdev user,id=net0,hostfwd=udp::7778-:7777 \
    -device qemu-xhci,id=usb \
    -device usb-kbd,bus=usb.0 \
    -device usb-mouse,bus=usb.0 \
    -display none \
    -serial file:"$SERIAL_LOG" \
    -no-reboot
