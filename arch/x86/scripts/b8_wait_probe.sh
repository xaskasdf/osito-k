#!/bin/bash
L=/root/osito-run/serial.log
for i in $(seq 1 14); do
  n=$(grep -ac 'FMW-POOL-SKIP' "$L" 2>/dev/null)
  sz=$(stat -c%s "$L" 2>/dev/null)
  echo "t=$((i*5))s skip=$n size=$sz"
  if [ "${n:-0}" -ge 6 ]; then break; fi
  sleep 5
done
echo "=== [FMW-POOL-SKIP] probe lines ==="
grep -anE '\[FMW-POOL-SKIP\]' "$L" 2>/dev/null | head -24
echo "=== VA-ALLOC lines (correlate blk to a base/size/caller) ==="
grep -anE '\[VA-ALLOC\]' "$L" 2>/dev/null | tail -40
