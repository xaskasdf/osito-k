#!/bin/bash
L=/root/osito-run/serial.log
echo "lines=$(wc -l < "$L") qemu=$(pgrep -fc 'qemu-system-x86_64.*55555')"
echo "Process crashed : $(grep -ac 'Process crashed' "$L")"
echo "B3 sig (0x1039B6CB|100000023) : $(grep -ac '1039B6CB\|100000023' "$L")"
echo "EXC32 vec=14 after boot (>18000) : $(awk 'NR>18000 && /EXC32.*vec=14/' "$L" | wc -l)"
echo "menu pump (PeekMessageA) : $(grep -ac 'PeekMessageA' "$L")"
echo "in-game markers (LoadMap/Botpack/Spawn) : $(grep -aciE 'LoadMap|Botpack|spawn|GameInfo' "$L")"
echo "=== any crash/exit markers after boot ==="
awk 'NR>18000' "$L" | grep -anaiE 'Process crashed|PE exit|EXC32 vec=14|Critical Error|RIP = 0x0xFFFF' | tail -8
echo "=== tail ==="
grep -av '^$' "$L" | tail -8
