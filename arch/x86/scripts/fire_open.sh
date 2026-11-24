#!/bin/bash
L=/root/osito-run/serial.log
B=$(wc -l < "$L")
echo "BEFORE=$B"
bash /mnt/c/Users/xasko/osito-k/arch/x86/scripts/ut_console_open.sh "${1:-DM-Deck16}"
sleep 3
A=$(wc -l < "$L")
echo "AFTER=$A delta=$((A-B))"
echo "qemu=$(pgrep -fc 'qemu-system-x86_64.*osito-run')"
echo "=== new serial lines ==="
sed -n "$((B+1)),$A p" "$L" | tail -80
