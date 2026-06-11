#!/bin/bash
L=/root/osito-run/serial.log
n1=$(wc -l < "$L")
sleep 3
n2=$(wc -l < "$L")
echo "lines: $n1 -> $n2 (delta $((n2-n1)) in 3s); qemu=$(pgrep -fc 'qemu-system-x86_64.*55555')"
echo "=== recent call targets / comparator / qsort ==="
grep -anaiE '10303765|1039B6|qsort|CAUSEINPUT|FAIL-FMT|THROWMSG|LoadMap|Character|NewGame|UMenu' "$L" | tail -10
echo "=== last 6 distinct non-CB32/INT2E lines (real progress) ==="
grep -avE 'CB32|INT2E callback|wcscpy' "$L" | grep -av '^$' | tail -8
