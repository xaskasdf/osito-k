#!/bin/bash
L=/root/osito-run/serial.log
echo "=== last THROWMSG / CXX-Fn (the new error text) ==="
grep -anE 'THROWMSG|CXX-F[0-9]' "$L" | tail -25
echo "=== last NOT FOUND files ==="
grep -anB1 -F 'NOT FOUND' "$L" | grep -aE "NtCreateFile|CreateFileW" | tail -15
echo "=== tail 30 ==="
tail -30 "$L"
