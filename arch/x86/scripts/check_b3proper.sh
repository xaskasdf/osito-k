#!/bin/bash
L=/root/osito-run/serial.log
echo "lines=$(wc -l < "$L") qemu=$(pgrep -fc 'qemu-system-x86_64.*55555')"
echo "=== B3 signatures (should be 0 in gameplay) ==="
echo "0x1039B6CB / 100000023 : $(grep -ac '1039B6CB\|100000023' "$L")"
echo "CR2=0x40 / paused      : $(grep -ac '0000000040\|paused' "$L")"
echo "qsort32 blob installed : $(grep -ac 'qsort32 blob' "$L")"
echo "=== gameplay reached ==="
echo "LoadMap/Botpack/spawn  : $(grep -aciE 'LoadMap|Botpack|spawn|GameInfo|Level is' "$L")"
echo "=== Preferences crash (B9) ==="
echo "Process crashed : $(grep -ac 'Process crashed' "$L")"
echo "EXC32 vec=14    : $(grep -ac 'EXC32.*vec=14' "$L")"
echo "CR2 0x10173/0x10295D30 (PE not-present): $(grep -acE '0x10173|10295D30' "$L")"
echo "=== last crash block ==="
ln=$(grep -anE 'EXC32.*vec=14|RIP = 0x0xFFFF8000021|Process crashed' "$L" | tail -1 | cut -d: -f1)
[ -n "$ln" ] && sed -n "$((ln-12)),$((ln+4))p" "$L" | grep -aiE 'RIP|CR2|CS |EXC32|dispatch|handler|not mapped|Process crashed'
echo "=== VM status ==="
