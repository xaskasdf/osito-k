#!/bin/bash
L=/root/osito-run/serial.log
for i in $(seq 1 16); do
  sz=$(stat -c%s "$L" 2>/dev/null)
  skip=$(grep -ac 'FMW-POOL-SKIP' "$L" 2>/dev/null)
  rtr=$(grep -ac 'FMW-ROUTER' "$L" 2>/dev/null)
  wcs=$(grep -ac 'wcscpy] src=L"0"' "$L" 2>/dev/null)
  echo "t=$((i*5))s size=$sz router=$rtr skip=$skip wcs0=$wcs"
  # stop once we're well past init (size stable + menu reached) or big
  if [ "${sz:-0}" -gt 1400000 ]; then break; fi
  sleep 5
done
echo "=== [FMW-ROUTER] installs ==="
grep -anE '\[FMW-ROUTER\]' "$L" 2>/dev/null | head
echo "=== [FMW-POOL-SKIP] (should be 0 if router caught them) ==="
grep -anE '\[FMW-POOL-SKIP\]' "$L" 2>/dev/null | head
echo "=== wcscpy L0 count (should be ~0) ==="
grep -ac 'wcscpy] src=L"0"' "$L" 2>/dev/null
echo "=== reached menu? ==="
grep -anE -i 'PeekMessage|RegisterClassExW.*Viewport|Browse|GameEngine' "$L" 2>/dev/null | tail -5
echo "=== any crash ==="
grep -anE 'EXCEPTION|Process crashed|triple|appError|appError|Assertion' "$L" 2>/dev/null | tail -6
