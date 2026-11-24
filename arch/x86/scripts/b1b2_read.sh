#!/bin/bash
L=/root/osito-run/serial.log
echo "=== size/mtime ==="; ls -la "$L"
echo "=== [KBD] (per key) last 30 ==="
grep -anE '\[KBD\]' "$L" | tail -30
echo "=== [MOU] (throttled) last 15 ==="
grep -anE '\[MOU\]' "$L" | tail -15
echo "=== capture/cursor signals after viewport (ShowCursor/SetCapture/ClipCursor/SetCursorPos) ==="
grep -anE -i 'ShowCursor|ClipCursor|SetCapture|SetCursorPos' "$L" | tail -15
echo "=== any crash/#PF ==="
grep -anE 'EXCEPTION|#PF|Process crashed|triple' "$L" | tail -6
echo "=== tail 6 ==="
tail -6 "$L"
