#!/bin/bash
L=/root/osito-run/serial.log
for i in $(seq 1 75); do
  alive=$(pgrep -fc 'qemu-system-x86_64.*osito-run')
  n=0; [ -f "$L" ] && n=$(wc -l < "$L")
  bt=0; [ -f "$L" ] && bt=$(grep -acF '[BT-0]' "$L")
  echo "t=${i} lines=$n bt0=$bt qemu=$alive"
  if [ "$bt" -ge 1 ]; then echo GOT_BT0; break; fi
  if [ "$n" -gt 1000 ] && [ "$alive" -eq 0 ]; then echo QEMU_DIED; break; fi
  sleep 4
done
