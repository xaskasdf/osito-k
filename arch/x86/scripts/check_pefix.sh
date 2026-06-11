#!/bin/bash
L=/root/osito-run/serial.log
echo "lines=$(wc -l < "$L") qemu=$(pgrep -fc 'qemu-system-x86_64.*55555')"
echo "PEFIX repaired : $(grep -ac 'PEFIX' "$L")"
echo "EXC32 vec=14   : $(grep -ac 'EXC32.*vec=14' "$L")"
echo "Process crashed: $(grep -ac 'Process crashed' "$L")"
echo "CR2=0x10295D30 : $(grep -ac '10295D30' "$L")"
echo "CR2=0x10173    : $(grep -ac '0x10173' "$L")"
echo "=== last PEFIX lines ==="
grep -anE 'PEFIX' "$L" | tail -6
echo "=== tail 22 non-empty ==="
grep -av '^$' "$L" | tail -22
