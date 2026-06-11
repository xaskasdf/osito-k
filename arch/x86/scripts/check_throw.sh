#!/bin/bash
# Inspect the UT99 cmdline-map boot serial log for the map-load throw message.
L=/root/osito-run/serial.log
if [ ! -f "$L" ]; then echo "NO SERIAL LOG"; exit 1; fi
echo "lines=$(wc -l < "$L")"
echo "=== qemu alive? ==="
pgrep -af qemu-system-x86_64 | head -2 || echo "QEMU DEAD"
echo "=== throw-message lines (CXX-F / THROWMSG / CXX-OBJ) ==="
grep -aE 'CXX-F[0-9]|THROWMSG|CXX-OBJ' "$L" | tail -40
echo "=== _CxxThrowException sites ==="
grep -aF '_CxxThrowException' "$L" | tail -20
echo "=== last 25 lines ==="
tail -25 "$L"
