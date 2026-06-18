#!/bin/bash
L=/root/osito-run/serial.log
echo "=== 2MB splits (Core.dll PT page PAs) ==="
grep -anE 'split 2MB' "$L" | head -25
echo "=== first #PF context (34420-34470) ==="
sed -n '34420,34470p' "$L" | grep -aiE 'RIP|CR2|EXC32|PF32|err='
echo "=== VA-ALLOC during Preferences (could trigger pt_alloc_page) — last 12 ==="
grep -anE '\[VA-ALLOC\]' "$L" | tail -12
