#!/bin/bash
L=/root/osito-run/serial.log
ln=$(grep -anE 'ExitProcess called' "$L" | tail -1 | cut -d: -f1)
# Find the cascade start: last _CxxThrowException with obj != 0x00000000 before exit
echo "=== all original throws (obj != 0) in last 3000 lines before exit ==="
sed -n "$((ln-3000)),$((ln))p" "$L" | grep -anE '_CxxThrowException: obj=0x0x[0-9A-F]*[1-9A-F]' | tail -5
echo "=== all appUnwindf labels before exit (innermost first ~ chronological) ==="
sed -n "$((ln-3000)),$((ln))p" "$L" | grep -anE 'arg0x1=L' | tail -25
echo "=== THROWMSG / CXX-Fn decodes ==="
sed -n "$((ln-3000)),$((ln))p" "$L" | grep -anE 'THROWMSG|CXX-Fn|appError|Critical' | tail -10
