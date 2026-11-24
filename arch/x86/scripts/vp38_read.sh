#!/bin/bash
L=/root/osito-run/serial.log
echo "size=$(stat -c%s "$L")"
echo "armed=$(grep -ac 'HWBP armed' "$L")"
echo "=== vp38 writer RIPs (histogram) ==="
grep -a 'HWBP. slot 0' "$L" | grep -oE 'RIP=0x[0-9a-fx]+' | sort | uniq -c | sort -rn | head -12
echo "=== first 8 hits (sequence) ==="
grep -a 'HWBP. slot 0' "$L" | head -8
echo "=== [CAP] flap context ==="
grep -a '\[CAP\] SetCapture' "$L" | tail -6
