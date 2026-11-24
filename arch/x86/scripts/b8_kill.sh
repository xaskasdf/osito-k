#!/bin/bash
# Kill qemu without self-matching (run as a file, so argv doesn't contain the pattern)
pkill -9 -f OVMF_VARS 2>/dev/null
sleep 2
if pgrep -f OVMF_VARS >/dev/null 2>&1; then echo "STILL ALIVE"; else echo "CLEAN"; fi
