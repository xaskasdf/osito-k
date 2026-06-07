#!/bin/bash
L=/root/osito-run/serial.log
echo "alive=$(pgrep -fc 'qemu-system-x86_64.*55555') lines=$(wc -l < "$L")"
echo "=== crash markers ==="
grep -anaiE 'EXC32|NULL-CALL|RET0-DIAG|BPDIAG|RECENT CALLS|\[rcall|Breakpoint|vector 3|Process crashed|SEH32. (empty|dispatch)|THROWMSG' "$L" 2>/dev/null | tail -30
echo
echo "=== context around last BPDIAG / RET0-DIAG / recent-calls ==="
ln=$(grep -anE 'BPDIAG|RET0-DIAG|recent native|RECENT' "$L" | tail -1 | cut -d: -f1)
if [ -n "$ln" ]; then sed -n "$((ln-40)),$((ln+40))p" "$L"; else echo "(no BPDIAG/RET0-DIAG marker found)"; fi
