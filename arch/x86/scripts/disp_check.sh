#!/bin/bash
L=/root/osito-run/serial.log
echo "size=$(stat -c%s "$L" 2>/dev/null)"
echo "=== ddraw activity: GetCaps / EnumDisplayModes / Set-GetDisplayMode ==="
grep -anE '\[DDRAW\] (GetCaps|EnumDisplayModes|SetDisplayMode|GetDisplayMode)' "$L" | head -20
echo "=== re-exec markers ==="
grep -anE 'RE-EXEC|CreateProcessA|ShellExecuteA' "$L" | head -8
echo "=== crash / exit ==="
grep -anE 'EXCEPTION|Process crashed|ExitProcess called|Execution finished' "$L" | tail -8
echo "=== tail 12 ==="
tail -12 "$L"
