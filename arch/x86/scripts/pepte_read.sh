#!/bin/bash
L=/root/osito-run/serial.log
echo "size=$(stat -c%s "$L" 2>/dev/null) pe_pte=$(grep -ac 'PE-PTE' "$L")"
echo "=== [PE-PTE] events (who unmaps/clears Core.dll PTEs) ==="
grep -anE '\[PE-PTE\]' "$L" | head -40
echo "=== crash / #PF on Core.dll range ==="
grep -anE 'EXCEPTION|Process crashed|CR2 = 0x0x0000000010(17|22)' "$L" | tail -8
echo "=== tail 6 ==="
tail -6 "$L"
