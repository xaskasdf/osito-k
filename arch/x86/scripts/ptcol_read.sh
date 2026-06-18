#!/bin/bash
L=/root/osito-run/serial.log
echo "size=$(stat -c%s "$L" 2>/dev/null) pt_collide=$(grep -ac 'PT-COLLIDE' "$L") pe_pte=$(grep -ac 'PE-PTE' "$L")"
echo "=== [PT-COLLIDE] events (free/alloc of live PT page) ==="
grep -anE '\[PT-COLLIDE\]' "$L" | head -30
echo "=== [VA-ALLOC] with [LOWPA-PT-RANGE] (backing in PT-page region) ==="
grep -anE 'LOWPA-PT-RANGE' "$L" | head -20
echo "=== [PE-PTE] (paging-API unmaps, expect 0) ==="
grep -anE '\[PE-PTE\]' "$L" | head -10
echo "=== crash #PF ==="
grep -anE 'EXC32|Process crashed' "$L" | tail -6
echo "=== tail 4 ==="
tail -4 "$L"
