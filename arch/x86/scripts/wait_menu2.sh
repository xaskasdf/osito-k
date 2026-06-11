#!/bin/bash
L=/root/osito-run/serial.log
prev=0; stable=0; sawalive=0; sawfresh=0
for i in $(seq 1 75); do
  alive=$(pgrep -fc 'qemu-system-x86_64.*osito-run')
  n=0; [ -f "$L" ] && n=$(wc -l < "$L")
  [ "$alive" -ge 1 ] && sawalive=1
  # consider serial "fresh" only once we've seen it small after a reset
  [ "$n" -lt 3000 ] && sawfresh=1
  echo "t=${i} lines=$n qemu=$alive fresh=$sawfresh"
  if [ "$sawalive" -eq 1 ] && [ "$alive" -eq 0 ]; then echo QEMU_DIED; break; fi
  if [ "$sawfresh" -eq 1 ] && [ "$n" -gt 17600 ]; then
    if [ "$n" -eq "$prev" ]; then stable=$((stable+1)); else stable=0; fi
    if [ "$stable" -ge 2 ]; then echo MENU_STABLE; break; fi
    prev=$n
  fi
  sleep 4
done
