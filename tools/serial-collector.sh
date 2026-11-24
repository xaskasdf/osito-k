#!/bin/bash
# OsitoK serial-collector.sh — listen on UDP and write to local serial.log.
#
# For bare-metal testing where the box doesn't have an attached serial cable,
# but DOES have ethernet. OsitoK's net stack (i211/e1000e) can blast serial
# output to a UDP port on the dev box.
#
# This requires a kernel-side change:
#   in arch/x86/main.c, after init, redirect kprintf to net_send_udp(...)
#   targeting <dev-box-ip>:7779.
#
# Usage:
#   tools/serial-collector.sh [port]
#   tools/serial-collector.sh 7779
#   tools/serial-collector.sh 7779 | grep -E "EXCEPTION|HELLO|FAIL"
#
# Output goes to stdout AND to ~/osito-k/arch/x86/build/serial.log.
#
# Requires:  socat or netcat-openbsd ('nc -u -l').

set -e

PORT="${1:-7779}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OSITOK_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
LOG="$OSITOK_ROOT/arch/x86/build/serial.log"

GREEN='\033[0;32m'
NC='\033[0m'
echo -e "${GREEN}[+]${NC} Listening on UDP port $PORT, mirror to $LOG"
echo -e "${GREEN}[+]${NC} OsitoK kernel must send kprintf to <this-host-ip>:$PORT"
echo "---"

if command -v socat > /dev/null; then
    socat -u UDP-RECV:$PORT - | tee -a "$LOG"
elif command -v nc > /dev/null; then
    # BSD netcat doesn't support UDP listen reliably; this works on Linux nc.
    nc -u -l -p $PORT 2>&1 | tee -a "$LOG"
else
    echo "ERR: neither socat nor nc found. Install socat:"
    echo "  macOS:  brew install socat"
    echo "  Linux:  apt-get install socat"
    exit 1
fi
