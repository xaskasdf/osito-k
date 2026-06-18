#!/bin/bash
L=/root/osito-run/serial.log
echo "=== vp38 writes with caller stack (set RIP 1012F674 / clear 1012F686) ==="
grep -a 'vp38' "$L" | grep -a 'stk:' | tail -16
echo "=== distinct code ptrs seen on stack ==="
grep -a 'vp38' "$L" | grep -oaE 'stk:.*' | grep -oaE '0x[0-9a-f]{6,8}' | sort | uniq -c | sort -rn | head -20
