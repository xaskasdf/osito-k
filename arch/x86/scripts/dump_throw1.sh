#!/bin/bash
L=/root/osito-run/serial.log
echo "lines=$(wc -l < "$L") qemu=$(pgrep -fc 'qemu-system-x86_64.*osito-run')"
echo "=== first UT-Logo-Map throw + 60 lines after ==="
ln=$(grep -anF "UT-Logo-Map.unr'" "$L" | head -1 | cut -d: -f1)
if [ -n "$ln" ]; then sed -n "$((ln-6)),$((ln+70))p" "$L"; else echo "no UT-Logo-Map throw found"; fi
echo
echo "=== any #PF / catch@1038e6 / exit after the throw? ==="
grep -anE "catch @0x0x1038E6|EXC32 vec=14 RIP=0x0x1038|PE process exited|NULL-DIAG|0x1038E6EE" "$L" | tail -20
