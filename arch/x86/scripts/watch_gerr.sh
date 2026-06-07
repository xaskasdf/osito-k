#!/bin/bash
L=/root/osito-run/serial.log
for i in $(seq 1 75); do
  alive=$(pgrep -fc 'qemu-system-x86_64.*osito-run')
  n=0; [ -f "$L" ] && n=$(wc -l < "$L")
  g=0; [ -f "$L" ] && g=$(grep -acF '[GERRHIST]' "$L")
  echo "t=${i} lines=$n gerrhist=$g qemu=$alive"
  if [ "$g" -ge 1 ]; then echo GOT_GERR; break; fi
  if [ "$n" -gt 1000 ] && [ "$alive" -eq 0 ]; then echo QEMU_DIED; break; fi
  sleep 4
done
echo "=== GERRHIST lines ==="
grep -anF '[GERRHIST]' "$L" 2>/dev/null | head -6
echo "=== throw 1 context ==="
grep -anF 'thrown_from=0x0x10903EE4' "$L" 2>/dev/null | head -2
