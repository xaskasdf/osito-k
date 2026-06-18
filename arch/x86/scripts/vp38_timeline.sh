#!/bin/bash
L=/root/osito-run/serial.log
echo "=== STAGE-A arm + vp38 write timeline (RIP + value) ==="
grep -aE 'STAGE-A|vp38' "$L" | tail -40
echo "=== distinct writer RIPs ==="
grep -a 'vp38' "$L" | grep -oE 'RIP=0x[0-9a-fx]+' | sort | uniq -c | sort -rn | head
echo "=== distinct values written ==="
grep -a 'vp38' "$L" | grep -oE 'val=0x[0-9a-fx]+' | sort | uniq -c | sort -rn | head
echo "=== SetCapture lines ==="
grep -a '\[CAP\] SetCapture' "$L" | tail -6
