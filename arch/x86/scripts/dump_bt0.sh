#!/bin/bash
L=/root/osito-run/serial.log
echo "=== BT-0 backtraces ==="
grep -anF '[BT-0]' "$L" | head -8
echo "=== context around first wcscpy-0 + BT-0 ==="
ln=$(grep -anF '[BT-0]' "$L" | head -1 | cut -d: -f1)
[ -n "$ln" ] && sed -n "$((ln-8)),$((ln+4))p" "$L"
