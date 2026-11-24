#!/bin/bash
LOG="${1:-/root/osito-run/serial.log}"
echo "=== lines: $(wc -l < "$LOG") ==="
L=$(grep -an 'EXCEPTION:' "$LOG" | tail -1 | cut -d: -f1)
if [ -z "$L" ]; then echo "no EXCEPTION; tail:"; tail -20 "$LOG"; exit 0; fi
echo "=== last EXCEPTION at line $L; [-6,+34] ==="
sed -n "$((L-6)),$((L+34))p" "$LOG"
echo "=== RCALL present? ==="
grep -ac 'RCALL' "$LOG"
