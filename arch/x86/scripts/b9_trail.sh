#!/bin/bash
# B9 fault-trail extractor — runs inside WSL osito distro
L=/root/osito-run/serial.log
echo "=== log size / mtime ==="
ls -la "$L"
echo "=== PEFIX count ==="
grep -ac 'PEFIX' "$L"
echo "=== PEFIX last 12 ==="
grep -anE 'PEFIX' "$L" | tail -12
echo "=== Preferences / pref markers ==="
grep -anE -i 'prefer|UWindowPreferences|UMenuOptionMenu' "$L" | tail -6
echo "=== last EXCEPTION/#PF/CR2 block ==="
grep -anE 'EXCEPTION|#PF|CR2 |Process crashed|triple|RESET|IST' "$L" | tail -20
echo "=== tail 30 ==="
tail -30 "$L"
