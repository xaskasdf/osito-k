#!/bin/bash
L=/root/osito-run/serial.log
echo "lines=$(wc -l < "$L") qemu=$(pgrep -fc 'qemu-system-x86_64.*osito-run')"
echo "=== UT-Logo-Map open lines (with following line) ==="
grep -anA1 -F "UT-Logo-Map.unr" "$L" | grep -aE "UT-Logo-Map|handle =|NOT FOUND" | head -12
echo "=== appThrow 1017D4B0 count (should be 0) ==="
grep -acF '1017D4B0' "$L"
echo "=== wcscpy L0 spin count (green-noise proxy) ==="
grep -acF 'src=L\"0\"' "$L"
echo "=== last 10 serial lines ==="
tail -10 "$L"
echo "=== screendump ==="
python3 /mnt/c/Users/xasko/osito-k/arch/x86/scripts/qemu_mon.py 127.0.0.1 55555 "screendump /root/osito-run/shot.ppm" >/dev/null 2>&1
ls -la /root/osito-run/shot.ppm 2>/dev/null
