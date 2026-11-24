#!/bin/bash
L=/root/osito-run/serial.log
echo "size=$(stat -c%s "$L")"
echo "=== capture sequence signals (SetCapture/ShowCursor/SetCursorPos/SetForeground after init) ==="
grep -anE -i 'SetCapture|ShowCursor|SetCursorPos|SetForegroundWindow|SPI_SETMOUSE|GetClientRect' "$L" | tail -15
echo "=== focus messages ==="
grep -anE 'KILLFOCUS|SETFOCUS|SetFocus' "$L" | tail -10
echo "=== WM_SIZE probe ==="
grep -anE 'WM_SIZE hwnd' "$L" | tail -6
echo "=== wcsicmp flood still? ==="
grep -anE 'WCSICMP#' "$L" | tail -3
echo "=== crashes ==="
grep -anE 'EXCEPTION|HALTED|NULL-CALL|Process crashed' "$L" | tail -4
echo "=== tail 8 ==="
tail -8 "$L"
