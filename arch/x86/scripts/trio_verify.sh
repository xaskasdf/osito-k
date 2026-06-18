#!/bin/bash
L=/root/osito-run/serial.log
echo "size=$(stat -c%s "$L")"
echo "GP=$(grep -ac 'General Protection' "$L") HALTED=$(grep -ac 'SYSTEM HALTED' "$L") NULLCALL=$(grep -ac 'NULL-CALL' "$L") PF=$(grep -ac 'EXCEPTION: #PF' "$L")"
grep -a 'reserved top region' "$L" | head -1
