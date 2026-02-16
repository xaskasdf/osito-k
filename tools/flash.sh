#!/bin/bash
#
# OsitoK - Flash script for Wemos D1
#
# Usage: ./tools/flash.sh [port]
#

PORT=${1:-COM4}
BAUD=460800
BUILD=build

echo "=== OsitoK Flash ==="
echo "Port: $PORT"
echo "Baud: $BAUD"
echo ""

if [ ! -f "$BUILD/osito.bin" ]; then
    echo "ERROR: Build artifacts not found. Run 'make' first."
    exit 1
fi

echo "Flashing..."
python -m esptool \
    --chip esp8266 \
    --port "$PORT" \
    --baud "$BAUD" \
    write_flash \
    --flash_mode dout \
    --flash_size 4MB \
    --flash_freq 40m \
    0x00000 "$BUILD/osito.bin"

if [ $? -eq 0 ]; then
    echo ""
    echo "Flash complete! Starting monitor..."
    echo "(Press Ctrl+] to exit)"
    echo ""
    python -m serial.tools.miniterm "$PORT" 115200
else
    echo ""
    echo "Flash FAILED!"
    echo "To restore original firmware:"
    echo "  python -m esptool --port $PORT write_flash 0x0 backup/wemos_d1_full_backup.bin"
    exit 1
fi
