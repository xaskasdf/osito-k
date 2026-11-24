#!/bin/bash
L=/root/osito-run/serial.log
for i in $(seq 1 60); do
  if [ -f "$L" ]; then
    n=$(wc -l < "$L")
    ebp=$(grep -acF 'CATCH-EBP' "$L")
    c38=$(grep -acF 'catch @0x0x1038' "$L")
    cf=$(grep -acE 'CXX-F[0-9]|THROWMSG' "$L")
    pkg=$(grep -acF "package" "$L")
    alive=$(pgrep -fc 'qemu-system-x86_64.*osito-run')
    echo "t=${i} lines=$n catchebp=$ebp catch1038=$c38 cxxf=$cf pkgmsg=$pkg qemu=$alive"
    if [ "$ebp" -gt 0 ]; then echo GOT_CATCH_EBP; break; fi
    if [ "$alive" -eq 0 ] && [ "$n" -gt 1000 ]; then echo QEMU_EXITED; break; fi
  fi
  sleep 4
done
