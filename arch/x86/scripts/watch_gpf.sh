#!/bin/bash
L=/root/osito-run/serial.log
for i in $(seq 1 40); do
  alive=$(pgrep -fc 'qemu-system-x86_64.*55555')
  n=0; [ -f "$L" ] && n=$(wc -l < "$L")
  px=0; [ -f "$L" ] && px=$(grep -ac 'PXDUMP' "$L")
  gpf=0; [ -f "$L" ] && gpf=$(grep -ac 'GetPixelFormat ->' "$L")
  echo "t=$i lines=$n pxdump=$px gpf=$gpf qemu=$alive"
  if [ "$px" -ge 4 ]; then echo GOT; break; fi
  if [ "$n" -gt 1000 ] && [ "$alive" -eq 0 ]; then echo QEMU_GONE; break; fi
  sleep 5
done
echo "=== GetPixelFormat + PXDUMP ==="
grep -anaiE 'GetPixelFormat ->|PXDUMP' "$L" 2>/dev/null | tail -12
