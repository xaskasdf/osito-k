#!/bin/bash
#
# OsitoK - Serial monitor
#
# Usage: ./tools/monitor.sh [port] [baud]
#

PORT=${1:-COM4}
BAUD=${2:-115200}

echo "=== OsitoK Monitor ==="
echo "Port: $PORT"
echo "Baud: $BAUD"
echo "(Press Ctrl+] to exit)"
echo ""

python -m serial.tools.miniterm "$PORT" "$BAUD"
