#!/bin/bash
# Wait for the menu pump, then watch for render-transition signatures while the
# user drives New Game. Reports when menu is ready and on any signature/exit.
L=/root/osito-run/serial.log
menu=0
for i in $(seq 1 120); do
  alive=$(pgrep -fc 'qemu-system-x86_64.*55555')
  n=0; [ -f "$L" ] && n=$(wc -l < "$L")
  pump=0; [ -f "$L" ] && pump=$(grep -ac 'PeekMessageA called' "$L")
  unh=0;  [ -f "$L" ] && unh=$(grep -ac 'Unhashed name' "$L")
  coop=0; [ -f "$L" ] && coop=$(grep -ac 'SetCooperativeLevel' "$L")
  thr=0;  [ -f "$L" ] && thr=$(grep -ac '0x10903EE4' "$L")
  crash=0;[ -f "$L" ] && crash=$(grep -ac 'Process crashed\|ExitProcess\|PE exit' "$L")
  if [ "$menu" = 0 ] && [ "$pump" -ge 1 ]; then
    menu=1; echo "t=$i MENU_READY (lines=$n) — drive New Game now"
  fi
  echo "t=$i lines=$n pump=$pump unhashed=$unh coop=$coop throw=$thr crash=$crash qemu=$alive"
  if [ "$unh" -ge 1 ] || [ "$coop" -ge 1 ]; then echo "SIGNATURE_HIT"; break; fi
  if [ "$n" -gt 1000 ] && [ "$alive" -eq 0 ]; then echo "QEMU_GONE"; break; fi
  sleep 5
done
echo "=== Unhashed name lines ==="
grep -an 'Unhashed name' "$L" 2>/dev/null | head -4
echo "=== SetCooperativeLevel context ==="
grep -an 'SetCooperativeLevel' "$L" 2>/dev/null | head -4
echo "=== tail ==="
grep -av '^$' "$L" 2>/dev/null | tail -8
