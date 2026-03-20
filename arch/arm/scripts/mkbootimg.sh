#!/bin/bash
#
# mkbootimg.sh -- Build boot.img for ROG Phone 5 (SM8350)
#
# Requires:
#   - mkbootimg (from Android tools)
#   - stock.dtb extracted from device
#
# Usage:
#   ./scripts/mkbootimg.sh
#   fastboot boot arch/arm/build/boot.img
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ARCH_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$ARCH_DIR/build"
IMAGE="$BUILD_DIR/Image"
DTB="$BUILD_DIR/stock.dtb"
BOOT_IMG="$BUILD_DIR/boot.img"

# Build SM8350 kernel
echo "[BUILD] make -C $ARCH_DIR PLATFORM=sm8350"
make -C "$ARCH_DIR" PLATFORM=sm8350 -j$(nproc)

if [ ! -f "$IMAGE" ]; then
    echo "ERROR: $IMAGE not found"
    exit 1
fi

echo "[INFO] Image: $(stat -c%s "$IMAGE") bytes"

if [ ! -f "$DTB" ]; then
    echo ""
    echo "WARNING: stock.dtb not found at $DTB"
    echo "Extract from device:"
    echo "  adb shell cat /dev/block/by-name/dtbo > dtbo.img"
    echo "  # or extract from stock boot.img with unpackbootimg"
    echo ""
    echo "For now, building without DTB (may not boot on real hardware)..."
    echo ""

    mkbootimg \
        --kernel "$IMAGE" \
        --ramdisk /dev/null \
        --base 0x00000000 \
        --kernel_offset 0x00080000 \
        --header_version 2 \
        --pagesize 4096 \
        -o "$BOOT_IMG"
else
    mkbootimg \
        --kernel "$IMAGE" \
        --ramdisk /dev/null \
        --dtb "$DTB" \
        --base 0x00000000 \
        --kernel_offset 0x00080000 \
        --header_version 2 \
        --pagesize 4096 \
        -o "$BOOT_IMG"
fi

echo ""
echo "[OK] $BOOT_IMG ($(stat -c%s "$BOOT_IMG") bytes)"
echo ""
echo "Flash (temporary boot, no permanent changes):"
echo "  fastboot boot $BOOT_IMG"
