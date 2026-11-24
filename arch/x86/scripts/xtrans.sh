#!/bin/bash
L=/root/osito-run/serial.log
echo "lines=$(wc -l < "$L")"
echo "=== last 45 lines before the final throw cascade ==="
ln=$(grep -anF 'thrown_from=0x0x10903EE4' "$L" | tail -1 | cut -d: -f1)
[ -n "$ln" ] && sed -n "$((ln-45)),$((ln+3))p" "$L"
