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
    make -C "$X86_DIR" -j$(nproc) 2>&1 | tail -5
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

info "ESP image: boot.efi=$(stat -c%s "$EFI_BIN") kernel.elf=$(stat -c%s "$KERN_BIN")"

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
    info "NVMe disk: $NVME_IMG ($(stat -c%s "$NVME_IMG") bytes)"
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
    -netdev user,id=net0,hostfwd=udp::7777-:7777 \
    -device qemu-xhci,id=usb \
    -device usb-kbd,bus=usb.0 \
    -device usb-mouse,bus=usb.0 \
    -audiodev wav,id=wav0,path=$BUILD_DIR/audio.wav \
    -device intel-hda,id=hda0 \
    -device hda-duplex,id=snd0,audiodev=wav0 \
    -vnc :0,password=on \
    -serial file:"$SERIAL_LOG" \
    -monitor unix:/tmp/qemu-monitor.sock,server,nowait \
    -no-reboot &

QEMU_PID=$!
sleep 1

# Set VNC password via QEMU monitor
echo "change vnc password osito" | socat - UNIX-CONNECT:/tmp/qemu-monitor.sock 2>/dev/null
info "VNC: connect to $(hostname -I | awk '{print $1}'):5900 (password: osito)"
info "  macOS: open vnc://\$(hostname -I | awk '{print \$1}'):5900"

wait $QEMU_PID
