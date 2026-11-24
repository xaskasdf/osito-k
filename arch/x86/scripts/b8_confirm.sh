#!/bin/bash
L=/root/osito-run/serial.log
echo "=== HeapAlloc / CRT / FMALLOC pool bases + early stub allocs ==="
grep -anE -i 'heap pool|crt pool|kmalloc|\[FMALLOC\]|HeapAlloc pool|pool @|pool base' "$L" 2>/dev/null | head -20
echo "=== FMALLOC stub allocations returning 0x019x or low addrs ==="
grep -anE '\[FMALLOC\]' "$L" 2>/dev/null | head -20
echo "=== anything mentioning 0x019A / 019A (the freed block region) ==="
grep -anE '019A[0-9A-Fa-f]{4}|0x019' "$L" 2>/dev/null | head -15
echo "=== GMSTATE handoff (stub -> real vtable CHANGED) ==="
grep -anE 'GMSTATE.*CHANGED|stub|gmalloc|GMalloc' "$L" 2>/dev/null | grep -aiE 'chang|install|takeover|stub' | head -12
