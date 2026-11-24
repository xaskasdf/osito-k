#!/bin/bash
L=/root/osito-run/serial.log
echo "=== mouse/cursor/dinput/HID markers in serial (tail) ==="
grep -anaiE 'mouse|cursor|dinput|directinput|WM_MOUSE|WM_LBUTTON|SetCursor|GetCursor|tablet|abs_|HID' "$L" 2>/dev/null | tail -30
echo "=== counts ==="
printf 'mouse: %s\n' "$(grep -aci 'mouse' "$L")"
printf 'cursor: %s\n' "$(grep -aci 'cursor' "$L")"
printf 'dinput/DirectInput: %s\n' "$(grep -aci 'dinput\|directinput' "$L")"
printf 'WM_MOUSE/LBUTTON: %s\n' "$(grep -aci 'WM_MOUSE\|WM_LBUTTON' "$L")"
printf 'LoadLibrary dinput: %s\n' "$(grep -ai 'LoadLibrary.*dinput' "$L" | head -2)"
