#!/bin/bash
L=/root/osito-run/serial.log
prev=0; stable=0
for i in $(seq 1 60); do
  if [ -f "$L" ]; then
    n=$(wc -l < "$L")
    alive=$(pgrep -fc 'qemu-system-x86_64.*osito-run')
    pm=$(grep -acE 'PeekMessage|GetMessage|RegisterClass' "$L")
    echo "t=${i} lines=$n peekmsg=$pm qemu=$alive"
    if [ "$alive" -eq 0 ]; then echo QEMU_DEAD; break; fi
    if [ "$n" -gt 17700 ] && [ "$n" -eq "$prev" ]; then stable=$((stable+1)); else stable=0; fi
    if [ "$stable" -ge 2 ]; then echo MENU_STABLE; break; fi
    prev=$n
  fi
  sleep 4
done
