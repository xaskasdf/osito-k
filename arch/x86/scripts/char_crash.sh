#!/bin/bash
L=/root/osito-run/serial.log
echo "alive=$(pgrep -fc 'qemu-system-x86_64.*55555') lines=$(wc -l < "$L")"
echo "=== Process crashed lines ==="
grep -an 'Process crashed' "$L" | head -6
echo "=== ExitProcess lines ==="
grep -an 'ExitProcess' "$L" | head -6
echo "=== PE exit lines ==="
grep -an 'PE exit' "$L" | head -6
echo "=== Unhashed / throw 0x10903EE4 ==="
grep -anc 'Unhashed name' "$L"
grep -anc '0x10903EE4' "$L"
echo "=== current tail (is it idling?) ==="
grep -av '^$' "$L" | tail -6
