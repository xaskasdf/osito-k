#!/bin/bash
L=/root/osito-run/serial.log
echo "=== EXC32 vec=14 RIP=0x13 occurrences ==="
grep -anE 'EXC32.*vec=14' "$L" | head -10
echo "=== NULL-CALL ==="
grep -anE 'NULL-CALL' "$L" | head -10
echo "=== PF32 rip ==="
grep -anE 'PF32. rip=' "$L" | head -10
echo "=== SEH32 empty chain / dispatch ==="
grep -anE 'SEH32.*(empty chain|dispatch code)' "$L" | head -12
echo "=== context 30 lines before FIRST EXC32 vec=14 ==="
f=$(grep -anE 'EXC32.*vec=14' "$L" | head -1 | cut -d: -f1)
[ -n "$f" ] && sed -n "$((f-30)),$((f+2))p" "$L"
