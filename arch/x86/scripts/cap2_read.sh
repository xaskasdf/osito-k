#!/bin/bash
L=/root/osito-run/serial.log
echo "size=$(stat -c%s "$L")"
echo "=== all [CAP] (capture sequence + focus samples) ==="
grep -a '\[CAP\]' "$L" | tail -25
echo "=== crash ==="
grep -anE 'EXCEPTION|HALTED|NULL-CALL|Process crashed|appError|Critical' "$L" | tail -6
ln=$(grep -anE 'EXCEPTION' "$L" | tail -1 | cut -d: -f1)
if [ -n "$ln" ]; then sed -n "$((ln)),$((ln+16))p" "$L" | grep -aE 'RIP|CR2|ERR|RAX|RBX|RSP'; fi
echo "=== last unwind labels ==="
grep -anE 'arg0x1=L' "$L" | tail -8
