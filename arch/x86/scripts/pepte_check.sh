#!/bin/bash
L=/root/osito-run/serial.log
for i in $(seq 1 18); do
  sz=$(stat -c%s "$L" 2>/dev/null)
  pe=$(grep -ac 'PE-PTE' "$L" 2>/dev/null)
  echo "t=$((i*5))s size=$sz pe_pte=$pe"
  if [ "${sz:-0}" -gt 1300000 ]; then break; fi
  sleep 5
done
echo "=== [PE-PTE] events (who unmaps Core.dll pages) ==="
grep -anE '\[PE-PTE\]' "$L" 2>/dev/null | head -40
echo "=== reached menu? ==="
grep -anE -i 'PeekMessage' "$L" 2>/dev/null | tail -2
