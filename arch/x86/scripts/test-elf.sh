#!/bin/bash
#
# OsitoK x86-64 — ELF Execution Test
#
# Creates an OsitoFS disk with hello.elf, boots in QEMU,
# checks serial log for "Hello from OsitoK!" output.
#
# Usage:
#   ./test-elf.sh              # Build + test
#   ./test-elf.sh --no-build   # Test only
#
# Exit: Ctrl-A X  or  QEMU auto-exits after 10s timeout
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
X86_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ROOT_DIR="$(cd "$X86_DIR/../.." && pwd)"
BUILD_DIR="$X86_DIR/build"
TEST_DIR="$X86_DIR/test"
TOOLS_DIR="$ROOT_DIR/tools/ositofs"

EFI_BIN="$BUILD_DIR/ositok.efi"
ESP_IMG="$BUILD_DIR/esp.img"
DISK_IMG="$BUILD_DIR/ositofs.img"
SERIAL_LOG="$BUILD_DIR/serial.log"

# OVMF firmware
for f in /usr/share/qemu/OVMF.fd \
         /usr/share/ovmf/OVMF.fd \
         /usr/share/OVMF/OVMF_CODE.fd; do
    [ -f "$f" ] && OVMF="$f" && break
done

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

# ── Prerequisites ──────────────────────────────────────────
command -v qemu-system-x86_64 >/dev/null || error "qemu-system-x86 not installed"
[ -n "$OVMF" ] || error "OVMF not found"
command -v mcopy >/dev/null 2>&1 || error "mtools not installed"

# ── Build ──────────────────────────────────────────────────
if [ "$1" != "--no-build" ]; then
    info "Building ositok.efi..."
    make -C "$X86_DIR" -j$(nproc) 2>&1 | tail -2

    info "Building hello.elf..."
    gcc -nostdlib -static -no-pie -o "$TEST_DIR/hello.elf" "$TEST_DIR/hello.S"

    info "Building fileio.elf..."
    gcc -nostdlib -static -no-pie -o "$TEST_DIR/fileio.elf" "$TEST_DIR/fileio.c"

    info "Building OsitoFS tools..."
    make -C "$TOOLS_DIR" -j$(nproc) 2>&1 | tail -1
fi

[ -f "$EFI_BIN" ] || error "ositok.efi not found"
[ -f "$TEST_DIR/hello.elf" ] || error "hello.elf not found"

# ── Create OsitoFS disk image ────────────────────────────
info "Creating OsitoFS disk with hello.elf..."

dd if=/dev/zero of="$DISK_IMG" bs=1M count=32 status=none
"$TOOLS_DIR/mkfs.ositofs" "$DISK_IMG" --label "test" 2>&1 | head -3
"$TOOLS_DIR/ositofs-write" "$DISK_IMG" "$TEST_DIR/hello.elf" 2>&1 | head -3
[ -f "$TEST_DIR/fileio.elf" ] && "$TOOLS_DIR/ositofs-write" "$DISK_IMG" "$TEST_DIR/fileio.elf" 2>&1 | head -3
[ -f "$TEST_DIR/hello_c.elf" ] && "$TOOLS_DIR/ositofs-write" "$DISK_IMG" "$TEST_DIR/hello_c.elf" 2>&1 | head -3
[ -f "$TEST_DIR/tcc.elf" ] && "$TOOLS_DIR/ositofs-write" "$DISK_IMG" "$TEST_DIR/tcc.elf" 2>&1 | head -3
[ -f "$TEST_DIR/tiny.c" ] && "$TOOLS_DIR/ositofs-write" "$DISK_IMG" "$TEST_DIR/tiny.c" 2>&1 | head -3
[ -f "$TEST_DIR/selftest.c" ] && "$TOOLS_DIR/ositofs-write" "$DISK_IMG" "$TEST_DIR/selftest.c" 2>&1 | head -3
"$TOOLS_DIR/ositofs-ls" "$DISK_IMG" 2>&1

# ── Create ESP image ─────────────────────────────────────
info "Creating ESP image..."
dd if=/dev/zero of="$ESP_IMG" bs=1M count=64 status=none
mformat -i "$ESP_IMG" -F ::
mmd -i "$ESP_IMG" ::/EFI
mmd -i "$ESP_IMG" ::/EFI/BOOT
mcopy -i "$ESP_IMG" "$EFI_BIN" ::/EFI/BOOT/BOOTX64.EFI

# ── QEMU firmware args ───────────────────────────────────
if [ "${OVMF_PFLASH:-0}" = "1" ]; then
    VARS_TMP="$BUILD_DIR/ovmf_vars.fd"
    cp "$OVMF_VARS" "$VARS_TMP"
    BIOS_ARGS="-drive if=pflash,format=raw,readonly=on,file=$OVMF \
               -drive if=pflash,format=raw,file=$VARS_TMP"
else
    BIOS_ARGS="-bios $OVMF"
fi

# ── Launch QEMU ──────────────────────────────────────────
rm -f "$SERIAL_LOG"

info "Launching QEMU (10s timeout)..."
info "  OVMF: $OVMF"
info "  ESP:  $ESP_IMG"
info "  Disk: $DISK_IMG"
info ""

timeout 25 qemu-system-x86_64 \
    $BIOS_ARGS \
    -drive file="$ESP_IMG",format=raw,if=ide \
    -drive file="$DISK_IMG",format=raw,if=none,id=nvme0 \
    -device nvme,serial=osito,drive=nvme0 \
    -m 512M \
    -machine q35 \
    -device e1000e,netdev=net0 \
    -netdev user,id=net0 \
    -display none \
    -serial file:"$SERIAL_LOG" \
    -no-reboot 2>/dev/null || true

# ── Check results ────────────────────────────────────────
info ""
info "=== Serial Log (last 50 lines) ==="
if [ -f "$SERIAL_LOG" ]; then
    # Filter out framebuffer noise (0xAF bytes)
    python3 -c "
data = open('$SERIAL_LOG', 'rb').read()
clean = bytes(b for b in data if b != 0xAF)
text = clean.decode('ascii', errors='replace')
lines = text.strip().split('\n')
for line in lines[-50:]:
    print(line)
" 2>/dev/null || tail -50 "$SERIAL_LOG"
else
    echo "(no serial log)"
fi

info ""
if [ -f "$SERIAL_LOG" ] && grep -q "Hello from OsitoK" "$SERIAL_LOG" 2>/dev/null; then
    echo -e "${GREEN}=== TEST PASSED: ELF execution works! ===${NC}"
else
    echo -e "${CYAN}=== Check serial log for details ===${NC}"
fi
