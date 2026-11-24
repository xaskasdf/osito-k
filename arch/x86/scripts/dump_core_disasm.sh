#!/bin/bash
CORE=/mnt/c/Users/xasko/osito-k/Core.dll
OBJ=/usr/bin/objdump
echo "=== outer_eip 0x1010796D ==="
$OBJ -d -M intel --start-address=0x10107900 --stop-address=0x101079E0 "$CORE" 2>/dev/null | grep -E '^\s+101' | sed -n '1,70p'
echo ""
echo "=== inner_eip 0x10122E80 (direct wcscpy caller area) ==="
$OBJ -d -M intel --start-address=0x10122E00 --stop-address=0x10122F00 "$CORE" 2>/dev/null | grep -E '^\s+101' | sed -n '1,70p'
