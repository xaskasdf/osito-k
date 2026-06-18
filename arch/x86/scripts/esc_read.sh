#!/bin/bash
L=/root/osito-run/serial.log
echo "=== [KBD-ESC] lifecycle ==="
grep -a '\[KBD-ESC\]' "$L" | head -20
echo "=== [CAP] flap with esc state ==="
grep -a '\[CAP\] SetCapture' "$L" | tail -10
echo "=== counts ==="
echo "flap_cycles=$(grep -ac 'smc_ret=0x0x10390159' "$L")"
