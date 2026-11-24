#!/bin/bash
# Show the engine activity right BEFORE a throw, filtering the repetitive
# SEH/CXX dispatch noise, to reveal what operation fails (appError reason).
LOG="${1:-/root/osito-run/serial.log}"
# line number of the LAST throw_from the EXE throw site
L=$(grep -an 'thrown_from=0x0x10903EE4\|thrown_from=0x10903EE4' "$LOG" | tail -1 | cut -d: -f1)
echo "=== last EXE throw at line $L ==="
echo "=== 60 lines before it, minus SEH/CXX/throw noise ==="
sed -n "$((L-60)),$((L))p" "$LOG" | grep -avE 'SEH32\]|^\[CXX\]|RtlRaiseException|_CxxThrowException|thrown type|CXX-OBJ|FS_BASE'
echo
echo "=== distinct non-noise log-line PREFIXES in the steady-state loop (last 4000 lines) ==="
tail -4000 "$LOG" | grep -avE 'SEH32\]|^\[CXX\]|RtlRaiseException|_CxxThrowException|thrown type|CXX-OBJ|FS_BASE' | grep -aoE '^\[[A-Za-z0-9_/-]+\]|^[A-Za-z].*:' | sort | uniq -c | sort -rn | head -25
