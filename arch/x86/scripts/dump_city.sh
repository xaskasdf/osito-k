#!/bin/bash
L=/root/osito-run/serial.log
echo "lines=$(wc -l < "$L")  qemu=$(pgrep -fc 'qemu-system-x86_64.*osito-run')"
echo "=== CATCH-EBP lines ==="
grep -anF 'CATCH-EBP' "$L"
echo "=== catch @0x1038 dispatch lines ==="
grep -anF 'catch @0x0x1038' "$L"
echo "=== throw sites (last 12) ==="
grep -anF '_CxxThrowException:' "$L" | tail -12
echo "=== first CATCH-EBP context (40 lines around it) ==="
ln=$(grep -anF 'CATCH-EBP' "$L" | head -1 | cut -d: -f1)
if [ -n "$ln" ]; then sed -n "$((ln-18)),$((ln+22))p" "$L"; fi
