#!/bin/bash
L=/root/osito-run/serial.log
echo "=== A1 held-keys this run (vk: 25=Left 26=Up 27=Right 28=Down) ==="
grep -a '\[A1\]' "$L" | sort | uniq -c
echo "=== flap cycles ==="
grep -ac 'smc_ret=0x0x10390159' "$L"
