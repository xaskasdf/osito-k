#!/bin/bash
LOG="${1:-/root/osito-run/serial.log}"
echo "=== total lines: $(wc -l < "$LOG") ==="
echo "FindFirstFileA calls: $(grep -ac 'FindFirstFileA' "$LOG")"
echo "FindNextFileA calls:  $(grep -ac 'FindNextFile' "$LOG")"
echo "FindClose calls:      $(grep -ac 'FindClose' "$LOG")"
echo "=== distinct FindFirst patterns (top 12) ==="
grep -aoE "FindFirstFileA: '[^']*'" "$LOG" | sort | uniq -c | sort -rn | head -12
echo "=== distinct FindFirst results (top 12) ==="
grep -aoE "FindFirst result: '[^']*'" "$LOG" | sort | uniq -c | sort -rn | head -12
echo "=== last 12 *.int FindFirst result lines ==="
grep -aE "FindFirst result|FindNext" "$LOG" | tail -12
