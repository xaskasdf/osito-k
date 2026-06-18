#!/bin/bash
L=/root/osito-run/serial.log
echo "size=$(stat -c%s "$L")"
echo "=== CauseInputEvent armed? ==="
grep -a 'armed CauseInputEvent' "$L" | tail -2
echo "=== [CIE] events (key + type: 1=Press 3=Release 4=Axis) ==="
grep -a '\[CIE\]' "$L" | tail -30
echo "=== distinct key/type seen ==="
grep -a '\[CIE\]' "$L" | sort | uniq -c | sort -rn | head -20
echo "=== last serial lines (hang check) ==="
tail -5 "$L"
