#!/bin/bash
LOG="${1:-/root/osito-run/serial.log}"
L=$(grep -an 'GErrorHist] G' "$LOG" | head -1 | cut -d: -f1)
echo "first ReadFile-beyond-EOF at line $L"
echo "=== last 15 file-open lines BEFORE the error ==="
awk -v lim="$L" 'NR<lim' "$LOG" | grep -aiE "CreateFileW|CreateFileA|NtCreateFile:" | tail -15
echo "=== any size/EndOfFile/Standard-info logging at all? ==="
grep -aciE "GetFileSize|QueryInformationFile|EndOfFile|FileStandard|AllocationSize" "$LOG"
echo "=== distinct file names opened (whole run) ==="
grep -aoE "NtCreateFile: '[^']*'" "$LOG" | sort | uniq -c | sort -rn | head -20
