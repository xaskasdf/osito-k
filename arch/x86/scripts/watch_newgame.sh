#!/bin/bash
# Baseline the current log, then watch ONLY for NEW render-transition crash
# signatures while the user drives New Game. coop/early-ExitProcess ignored.
L=/root/osito-run/serial.log
base=$(wc -l < "$L")
echo "baseline=$base — drive New Game now; watching for Unhashed/throw/crash"
for i in $(seq 1 90); do
  alive=$(pgrep -fc 'qemu-system-x86_64.*55555')
  n=$(wc -l < "$L")
  # only count signatures AFTER baseline
  new=$(tail -n +"$((base+1))" "$L")
  unh=$(printf '%s' "$new" | grep -ac 'Unhashed name')
  thr=$(printf '%s' "$new" | grep -ac '0x10903EE4')
  crash=$(printf '%s' "$new" | grep -ac 'Process crashed')
  exitp=$(printf '%s' "$new" | grep -ac 'ExitProcess')
  echo "t=$i lines=$n(+$((n-base))) unhashed=$unh throw=$thr crash=$crash exitproc=$exitp qemu=$alive"
  if [ "$unh" -ge 1 ] || [ "$thr" -ge 1 ] || [ "$crash" -ge 1 ]; then echo "CRASH_SIG"; break; fi
  if [ "$alive" -eq 0 ]; then echo "QEMU_GONE"; break; fi
  sleep 6
done
echo "=== NEW Unhashed lines ==="
tail -n +"$((base+1))" "$L" | grep -an 'Unhashed name' | head -4
echo "=== NEW throw/Critical/appError ==="
tail -n +"$((base+1))" "$L" | grep -anaiE '0x10903EE4|Critical Error|appError|LoadMap|LocalMapURL|Browse' | head -10
echo "=== tail ==="
tail -n +"$((base+1))" "$L" | grep -av '^$' | tail -12
