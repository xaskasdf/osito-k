#!/bin/bash
#
# OsitoK x86-64 — QEMU Test Script
#
# Boots ositok.efi in QEMU with UEFI + e1000e NIC.
# No root required. Uses user-mode networking with UDP port forward.
#
# Usage:
#   ./qemu-test.sh              # Build + boot
#   ./qemu-test.sh --no-build   # Boot only (skip build)
#
# Test from another terminal:
#   echo "hola osito" | nc -u localhost 7777
#
# Exit QEMU: Ctrl-A X
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
X86_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$X86_DIR/build"
EFI_BIN="$BUILD_DIR/boot.efi"
KERN_BIN="$BUILD_DIR/kernel.elf"
ESP_IMG="$BUILD_DIR/esp.img"

# OVMF firmware paths (try common locations)
# Prefer the combined OVMF.fd (works with -bios), then split CODE+VARS (needs pflash)
for f in /usr/share/qemu/OVMF.fd \
         /usr/share/ovmf/OVMF.fd \
         /usr/share/OVMF/OVMF_CODE.fd; do
    [ -f "$f" ] && OVMF="$f" && break
done

# If only 4M split firmware available, use pflash mode
if [ -z "$OVMF" ] && [ -f /usr/share/OVMF/OVMF_CODE_4M.fd ]; then
    OVMF="/usr/share/OVMF/OVMF_CODE_4M.fd"
    OVMF_VARS="/usr/share/OVMF/OVMF_VARS_4M.fd"
    OVMF_PFLASH=1
fi

# macOS: Homebrew QEMU ships edk2 split firmware
if [ -z "$OVMF" ]; then
    QEMU_SHARE="$(qemu-system-x86_64 --version 2>/dev/null | head -1 | sed 's/.*version //')"
    for d in /usr/local/Cellar/qemu/*/share/qemu \
             /opt/homebrew/Cellar/qemu/*/share/qemu \
             /opt/homebrew/share/qemu \
             /usr/local/share/qemu; do
        if [ -f "$d/edk2-x86_64-code.fd" ]; then
            OVMF="$d/edk2-x86_64-code.fd"
            # Use i386-vars (compatible with x86_64) or x86_64-specific if present
            for varfile in "$d/edk2-i386-vars.fd" "$d/edk2-x86_64-vars.fd"; do
                [ -f "$varfile" ] && OVMF_VARS="$varfile" && break
            done
            OVMF_PFLASH=1
            break
        fi
    done
fi

RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
NC='\033[0m'

info()  { echo -e "${GREEN}[+]${NC} $*"; }
error() { echo -e "${RED}[!]${NC} $*"; exit 1; }

# ── Check prerequisites ──────────────────────────────────────

command -v qemu-system-x86_64 >/dev/null || error "qemu-system-x86 not installed. Run: sudo apt install qemu-system-x86"
[ -n "$OVMF" ] || error "OVMF not found. Run: sudo apt install ovmf"
command -v mtools >/dev/null 2>&1 || command -v mcopy >/dev/null 2>&1 || error "mtools not installed. Run: sudo apt install mtools"

# ── Build if needed ───────────────────────────────────────────

if [ "$1" != "--no-build" ]; then
    info "Building boot.efi + kernel.elf..."
    # On macOS, use cross-compiler and macOS gnu-efi paths
    if [ "$(uname)" = "Darwin" ]; then
        GNU_EFI="/private/tmp/gnu-efi"
        make -C "$X86_DIR" -j$(sysctl -n hw.ncpu) \
            CC=x86_64-elf-gcc LD=x86_64-elf-ld OBJCOPY=x86_64-elf-objcopy \
            EFI_INC="$GNU_EFI/inc" EFI_INC_ARCH="$GNU_EFI/inc/x86_64" \
            EFI_LIB="$GNU_EFI/x86_64/lib" \
            EFI_CRT="$GNU_EFI/x86_64/gnuefi/crt0-efi-x86_64.o" \
            EFI_LDS="$GNU_EFI/gnuefi/elf_x86_64_efi.lds" \
            2>&1 | tail -5
    else
        make -C "$X86_DIR" -j$(nproc) 2>&1 | tail -5
    fi
fi

[ -f "$EFI_BIN" ]  || error "boot.efi not found at $EFI_BIN"
[ -f "$KERN_BIN" ] || error "kernel.elf not found at $KERN_BIN"

# ── Create ESP image ─────────────────────────────────────────

info "Creating ESP image..."
dd if=/dev/zero of="$ESP_IMG" bs=1M count=64 status=none
mformat -i "$ESP_IMG" -F ::
mmd -i "$ESP_IMG" ::/EFI
mmd -i "$ESP_IMG" ::/EFI/BOOT
mcopy -i "$ESP_IMG" "$EFI_BIN" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$ESP_IMG" "$KERN_BIN" ::/EFI/BOOT/kernel.elf
# startup.nsh: auto-boot via UEFI Shell (needed when VARS lacks boot order)
printf 'FS0:\\EFI\\BOOT\\BOOTX64.EFI\r\n' > /tmp/startup.nsh
mcopy -i "$ESP_IMG" /tmp/startup.nsh ::/startup.nsh

FSIZE() { stat -c%s "$1" 2>/dev/null || stat -f%z "$1" 2>/dev/null || wc -c < "$1"; }
info "ESP image: boot.efi=$(FSIZE "$EFI_BIN") kernel.elf=$(FSIZE "$KERN_BIN")"

# ── Launch QEMU ──────────────────────────────────────────────

info "Launching QEMU..."
info "  OVMF: $OVMF"
info "  NIC:  e1000e (Intel 82574L, igb family)"
info "  USB:  xHCI + keyboard + mouse"
info "  Net:  user-mode, UDP :7777 → guest 10.0.2.15:7777"
info ""
echo -e "${CYAN}  Test: echo \"hola osito\" | nc -u localhost 7777${NC}"
echo -e "${CYAN}  Exit: Ctrl-A X${NC}"
info ""

# Build QEMU firmware args
if [ "${OVMF_PFLASH:-0}" = "1" ]; then
    # Copy VARS to temp (pflash needs writable vars)
    VARS_TMP="$BUILD_DIR/ovmf_vars.fd"
    cp "$OVMF_VARS" "$VARS_TMP"
    BIOS_ARGS="-drive if=pflash,format=raw,readonly=on,file=$OVMF \
               -drive if=pflash,format=raw,file=$VARS_TMP"
else
    BIOS_ARGS="-bios $OVMF"
fi

SERIAL_LOG="$BUILD_DIR/serial.log"
rm -f "$SERIAL_LOG"
info "Serial log: $SERIAL_LOG"
info "  (tail -f $SERIAL_LOG to watch)"

# NVMe disk image (OsitoFS)
NVME_IMG="$BUILD_DIR/nvme.img"
NVME_ARGS=""
if [ -f "$NVME_IMG" ]; then
    info "NVMe disk: $NVME_IMG ($(FSIZE "$NVME_IMG") bytes)"
    NVME_ARGS="-drive file=$NVME_IMG,format=raw,if=none,id=nvme0,cache=none -device nvme,serial=deadbeef,drive=nvme0"
fi

# Display: cocoa native window on macOS, VNC fallback on Linux
if [ "$(uname)" = "Darwin" ]; then
    DISPLAY_ARGS="-display cocoa,zoom-to-fit=off"
    info "Display: native macOS window (cocoa)"
else
    DISPLAY_ARGS="-vnc :0,password=on"
fi

qemu-system-x86_64 \
    $BIOS_ARGS \
    -drive file="$ESP_IMG",format=raw,if=ide \
    $NVME_ARGS \
    -m 512M \
    -machine q35 \
    -smp 4 \
    -device e1000e,netdev=net0 \
    -netdev user,id=net0,hostfwd=udp::7777-:7777 \
    -device qemu-xhci,id=usb \
    -device usb-kbd,bus=usb.0 \
    -device usb-mouse,bus=usb.0 \
    -audiodev wav,id=wav0,path=$BUILD_DIR/audio.wav \
    -device intel-hda,id=hda0 \
    -device hda-duplex,id=snd0,audiodev=wav0 \
    $DISPLAY_ARGS \
    -serial file:"$SERIAL_LOG" \
    -monitor unix:/tmp/qemu-monitor.sock,server,nowait \
    -no-reboot

wait
