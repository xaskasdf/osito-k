#!/bin/bash
# Emit one line per poll: boot progress + whether the map-load throw/hang appeared.
L=/root/osito-run/serial.log
for i in $(seq 1 50); do
  if [ -f "$L" ]; then
    n=$(wc -l < "$L")
    cxx=$(grep -acE 'CXX-F[0-9]|THROWMSG' "$L")
    pkg0=$(grep -acF 'src=L"0"' "$L")
    crit=$(grep -acE 'Critical|appError|Can' "$L")
    alive=$(pgrep -fc qemu-system-x86_64)
    echo "t=${i} lines=$n cxxmsg=$cxx pkg0loop=$pkg0 crit=$crit qemu=$alive"
    if [ "$cxx" -gt 0 ]; then echo "GOT_CXX_MSG"; break; fi
    if [ "$pkg0" -gt 20 ]; then echo "PKG0_HANG"; break; fi
    if [ "$alive" -eq 0 ] && [ "$n" -gt 1000 ]; then echo "QEMU_EXITED"; break; fi
  fi
  sleep 4
done
