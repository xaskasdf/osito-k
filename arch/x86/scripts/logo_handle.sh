#!/bin/bash
L=/root/osito-run/serial.log
echo "=== UT-Logo-Map.unr NtCreateFile open + result (handle vs NOT FOUND) ==="
grep -anA2 "NtCreateFile: 'UT-Logo-Map.unr'" "$L" | grep -aE "NtCreateFile: 'UT-Logo|handle =|NOT FOUND" | tail -10
echo "=== RenderWorld / LoadMap / Level markers ==="
grep -anE "LoadMap|RenderWorld|Level is|spawn|Bringing|Game class" "$L" | tail -8
echo "=== re-screendump ==="
python3 /mnt/c/Users/xasko/osito-k/arch/x86/scripts/qemu_mon.py 127.0.0.1 55555 "screendump /root/osito-run/shot2.ppm" >/dev/null 2>&1
python3 /mnt/c/Users/xasko/osito-k/arch/x86/scripts/ppm2png.py /root/osito-run/shot2.ppm /mnt/c/Users/xasko/osito-k/arch/x86/build/shot2.png 2>&1 | tail -1
