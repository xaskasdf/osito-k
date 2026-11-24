#!/bin/bash
L=/root/osito-run/serial.log
echo "=== the single find/Failed-load line + context ==="
grep -anaiE 'cant find|failed to load' "$L" | head -5
echo
echo "=== first vs last occurrence line# of src=L\"0\" ==="
grep -an 'src=L.0.' "$L" | head -1
grep -an 'src=L.0.' "$L" | tail -1
echo
echo "=== total log lines ==="
wc -l < "$L"
echo "=== any 'None None' or '.GameEngine' fallback strings? ==="
printf 'None None : %s\n' "$(grep -ac 'None None' "$L")"
printf '.GameEngine fallback : %s\n' "$(grep -ac 'Failed to load.*GameEngine' "$L")"
echo "=== appError / Critical markers ==="
grep -anaiE 'appError|Critical Error|GIsCriticalError' "$L" | tail -6
