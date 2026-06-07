#!/bin/bash
L=/root/osito-run/serial.log
for i in $(seq 1 40); do
  alive=$(pgrep -fc 'qemu-system-x86_64.*55555')
  n=0; [ -f "$L" ] && n=$(wc -l < "$L")
  px=0; [ -f "$L" ] && px=$(grep -ac 'PXDUMP' "$L")
  echo "t=$i lines=$n pxdump=$px qemu=$alive"
  if [ "$px" -ge 4 ]; then echo GOT_PXDUMP; break; fi
  if [ "$n" -gt 1000 ] && [ "$alive" -eq 0 ]; then echo QEMU_GONE; break; fi
  sleep 5
done
echo "=== PXDUMP + SetDisplayMode ==="
grep -anaiE 'PXDUMP|SetDisplayMode|created surface|Using GOP' "$L" 2>/dev/null | tail -20
