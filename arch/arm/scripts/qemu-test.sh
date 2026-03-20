#!/bin/bash
#
# qemu-test.sh -- Launch Osito-K AArch64 on QEMU virt
#
# Usage:
#   ./scripts/qemu-test.sh              # build + run
#   ./scripts/qemu-test.sh --no-build   # run only (Image must exist)
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ARCH_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
IMAGE="$ARCH_DIR/build/Image"

# Check QEMU
if ! command -v qemu-system-aarch64 &>/dev/null; then
    echo "ERROR: qemu-system-aarch64 not found. Install with:"
    echo "  sudo apt install qemu-system-arm"
    exit 1
fi

# Build unless --no-build
if [ "$1" != "--no-build" ]; then
    echo "[BUILD] make -C $ARCH_DIR PLATFORM=virt"
    make -C "$ARCH_DIR" PLATFORM=virt
fi

if [ ! -f "$IMAGE" ]; then
    echo "ERROR: $IMAGE not found. Run 'make -C $ARCH_DIR PLATFORM=virt' first."
    exit 1
fi

echo "[QEMU] Launching Osito-K AArch64 (virt)..."
echo "       Press Ctrl-A X to exit QEMU."
echo ""

exec qemu-system-aarch64 \
    -M virt,gic-version=3 \
    -cpu cortex-a72 \
    -m 512M \
    -nographic \
    -kernel "$IMAGE" \
    -no-reboot
