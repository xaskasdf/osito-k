#!/bin/bash
L=/root/osito-run/serial.log
echo "=== serial size/mtime ==="; ls -la "$L"
echo
echo "=== capture/cursor signals (drives mouselook_active) ==="
grep -anE -i 'ShowCursor|ClipCursor|SetCapture|SetCursorPos|GetCursorPos|capture_hwnd|mouselook' "$L" | tail -30
echo
echo "=== viewport window create / class ==="
grep -anE -i 'ViewportWindow|RegisterClass|CreateWindowEx' "$L" | tail -20
echo
echo "=== USER32 input routing (WM_KEY / WM_MOUSE / target) ==="
grep -anE -i 'WM_KEYDOWN|WM_KEYUP|WM_MOUSEMOVE|post_keyboard|post_mouse|input_target|\[USER32\]' "$L" | tail -30
echo
echo "=== last 15 lines (where is it now) ==="
tail -15 "$L"
