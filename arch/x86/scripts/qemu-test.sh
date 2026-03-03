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
EFI_BIN="$BUILD_DIR/ositok.efi"
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
    info "Building ositok.efi..."
    make -C "$X86_DIR" -j$(nproc) 2>&1 | tail -3
fi

[ -f "$EFI_BIN" ] || error "ositok.efi not found at $EFI_BIN"

# ── Create ESP image ─────────────────────────────────────────

info "Creating ESP image..."
dd if=/dev/zero of="$ESP_IMG" bs=1M count=64 status=none
mformat -i "$ESP_IMG" -F ::
mmd -i "$ESP_IMG" ::/EFI
mmd -i "$ESP_IMG" ::/EFI/BOOT
mcopy -i "$ESP_IMG" "$EFI_BIN" ::/EFI/BOOT/BOOTX64.EFI

info "ESP image: $ESP_IMG ($(stat -c%s "$EFI_BIN") bytes EFI)"

# ── Launch QEMU ──────────────────────────────────────────────

info "Launching QEMU..."
info "  OVMF: $OVMF"
info "  NIC:  e1000e (Intel 82574L, igb family)"
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

qemu-system-x86_64 \
    $BIOS_ARGS \
    -drive file="$ESP_IMG",format=raw,if=ide \
    -m 512M \
    -machine q35 \
    -device e1000e,netdev=net0 \
    -netdev user,id=net0,hostfwd=udp::7777-:7777 \
    -display none \
    -serial file:"$SERIAL_LOG" \
    -no-reboot
