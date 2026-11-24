#!/bin/bash
L=/root/osito-run/serial.log
echo "size=$(stat -c%s "$L" 2>/dev/null) routers=$(grep -ac FMW-ROUTER "$L") skip=$(grep -ac FMW-POOL-SKIP "$L")"
echo "=== FMW-ROUTER ==="
grep -anE 'FMW-ROUTER' "$L" | head
echo "=== FMW-POOL-SKIP (tail) ==="
grep -anE 'FMW-POOL-SKIP' "$L" | tail -8
echo "=== crashes/exceptions/appError ==="
grep -anE 'EXCEPTION|Process crashed|triple|appError|Assertion|Critical' "$L" | tail -10
echo "=== Preferences markers ==="
grep -anE -i 'prefer|UMenuOption|video.*mode|resolution' "$L" | tail -8
echo "=== tail 15 ==="
tail -15 "$L"
