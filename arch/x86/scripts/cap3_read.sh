#!/bin/bash
L=/root/osito-run/serial.log
echo "size=$(stat -c%s "$L")"
echo "=== [CAP] trail (SetFocus flips + capture sequence + GetFocus samples) ==="
grep -a '\[CAP\]' "$L" | head -40
echo "..."
grep -a '\[CAP\]' "$L" | tail -10
echo "=== counts ==="
echo "setfocus_flips=$(grep -ac 'CAP. SetFocus' "$L") setcapture=$(grep -ac 'CAP. SetCapture' "$L") showcursor=$(grep -ac 'CAP. ShowCursor' "$L")"
