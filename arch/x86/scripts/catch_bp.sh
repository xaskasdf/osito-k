#!/bin/bash
L=/root/osito-run/serial.log
echo "alive=$(pgrep -fc 'qemu-system-x86_64.*55555') lines=$(wc -l < "$L")"
echo "=== #BP / vec=3 / RET0-DIAG / recent-calls / crash markers ==="
grep -anaiE 'vec=3|#BP|breakpoint|RET0-DIAG|RECENT|recent native|EXC32|NULL-CALL|Process crashed|PE exit|SEH32' "$L" 2>/dev/null | tail -40
echo
echo "=== context around first RET0-DIAG (or vec=3) ==="
ln=$(grep -anE 'RET0-DIAG|vec=3' "$L" | head -1 | cut -d: -f1)
[ -n "$ln" ] && sed -n "$((ln-6)),$((ln+45))p" "$L"
