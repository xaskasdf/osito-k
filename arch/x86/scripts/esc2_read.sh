#!/bin/bash
L=/root/osito-run/serial.log
echo "=== A1 fires (ESC stuck -> many 0x1B lines) ==="
grep -a '\[A1\]' "$L" | sort | uniq -c
echo "=== KBD-ESC down/up lifecycle (last 14) ==="
grep -a 'KBD-ESC' "$L" | tail -14
echo "=== KBD-ESC down vs up counts ==="
echo "DOWN: $(grep -ac 'KBD-ESC. DOWN' "$L")  UP: $(grep -ac 'KBD-ESC. UP' "$L")"
