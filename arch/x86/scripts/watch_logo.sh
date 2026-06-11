#!/bin/bash
L=/root/osito-run/serial.log
for i in $(seq 1 75); do
  alive=$(pgrep -fc 'qemu-system-x86_64.*osito-run')
  n=0; [ -f "$L" ] && n=$(wc -l < "$L")
  found=0; notfound=0; thr=0
  if [ -f "$L" ]; then
    found=$(grep -aF 'UT-Logo-Map.unr' "$L" | grep -acF 'handle =')
    notfound=$(grep -aF "UT-Logo-Map.unr'" "$L" | grep -B0 -aA1 -F 'UT-Logo-Map' "$L" >/dev/null 2>&1; grep -aA1 -F "UT-Logo-Map.unr'" "$L" | grep -acF 'NOT FOUND')
    thr=$(grep -acF '1017D4B0' "$L")
  fi
  echo "t=${i} lines=$n logo_open_attempts=$(grep -acF "UT-Logo-Map.unr'" "$L" 2>/dev/null) notfound=$notfound appthrow1017=$thr qemu=$alive"
  if [ "$(grep -acF "UT-Logo-Map.unr'" "$L" 2>/dev/null)" -gt 0 ]; then echo LOGO_TOUCHED; fi
  if [ "$n" -gt 1000 ] && [ "$alive" -eq 0 ]; then echo QEMU_DIED; break; fi
  if [ "$i" -ge 14 ] && [ "$n" -gt 17600 ]; then echo MENU_REACHED; break; fi
  sleep 4
done
echo "=== UT-Logo-Map open results ==="
grep -aA1 -F "UT-Logo-Map.unr'" "$L" 2>/dev/null | grep -aE 'NOT FOUND|handle =' | head -6
