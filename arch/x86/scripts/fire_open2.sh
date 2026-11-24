#!/bin/bash
L=/root/osito-run/serial.log
MAP="${1:-DM-Deck16}"
B=$(wc -l < "$L")
echo "BEFORE=$B map=$MAP"
bash /mnt/c/Users/xasko/osito-k/arch/x86/scripts/ut_console_open.sh "$MAP" >/dev/null 2>&1
sleep 4
A=$(wc -l < "$L")
echo "AFTER=$A delta=$((A-B)) qemu=$(pgrep -fc 'qemu-system-x86_64.*osito-run')"
echo "=== #1 signatures in the delta (filtering out the 0-spin) ==="
sed -n "$((B+1)),$A p" "$L" | grep -aiE "1017D4B0|Browse|ClientTravel|LocalMapURL|$MAP|Failed|find file|find package|CXX-F|THROWMSG|catch @0x0x1038|EXC32|#PF" | head -50
echo "=== throw sites in delta ==="
sed -n "$((B+1)),$A p" "$L" | grep -aF '_CxxThrowException:' | head -20
