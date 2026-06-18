#!/bin/bash
L=/root/osito-run/serial.log
echo "=== [KR] keyboard reports (k0=0x29 is ESC) — last 30 ==="
grep -a '\[KR\]' "$L" | tail -30
echo "=== KR total + ESC-bearing vs ESC-absent ==="
echo "total KR: $(grep -ac '\[KR\]' "$L")"
echo "k0=0x29 (ESC): $(grep -ac '\[KR\] n=.* k0=0x0x29' "$L")"
echo "n=0 (all released): $(grep -ac '\[KR\] n=0 ' "$L")"
echo "=== ESC lifecycle ==="
echo "DOWN: $(grep -ac 'KBD-ESC. DOWN' "$L")  UP: $(grep -ac 'KBD-ESC. UP' "$L")"
