#!/bin/bash
L=/root/osito-run/serial.log
echo "=== A1 stuck-key hits (vk histogram) ==="
grep -a '\[A1\]' "$L" | sort | uniq -c | head -20
echo "=== A2 keys-down at flap release ==="
grep -a 'rel#' "$L" | tail -8
echo "=== ESC lifecycle ==="
grep -a 'KBD-ESC' "$L" | tail -10
echo "=== flap count ==="
grep -ac 'smc_ret=0x0x10390159' "$L"
