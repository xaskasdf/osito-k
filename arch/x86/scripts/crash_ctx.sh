#!/bin/bash
LOG="${1:-/root/osito-run/serial.log}"
if [ ! -f "$LOG" ]; then echo "NO LOG: $LOG"; ls -la "$(dirname "$LOG")" 2>/dev/null; exit 1; fi
echo "=== log lines: $(wc -l < "$LOG") ==="
L=$(grep -an 'Critical Error' "$LOG" | tail -1 | cut -d: -f1)
if [ -z "$L" ]; then echo "no Critical Error found; tail:"; tail -40 "$LOG"; exit 0; fi
echo "=== Critical Error at line $L; context [-120,+3] (noise filtered) ==="
sed -n "$((L-120)),$((L+3))p" "$LOG" | grep -avE 'WCSICMP|\[wcscpy|\[wcslen| size =|FreeType = |tombstone| release |NtAllocateVirtualMemory: |NtFreeVirtualMemory: '
