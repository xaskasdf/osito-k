#!/bin/bash
L=/root/osito-run/serial.log
echo "=== display-mode enumeration calls (which path UT uses) ==="
grep -anE -i 'EnumDisplayModes|EnumDisplaySettings|ChangeDisplaySettings|\[DDRAW\] (Enum|SetDisplayMode|GetDisplayMode)|GetDeviceCaps|EnumModes' "$L" | head -30
echo ""
echo "=== SetDisplayMode requests (resolution/depth changes) ==="
grep -anE -i 'SetDisplayMode req|SetDisplayMode|bpp|0x20 .*depth|depth' "$L" | tail -20
echo ""
echo "=== context around process exit (depth-change crash) ==="
grep -anE 'Execution finished|PE process exited|appError|GetDisplayMode|EnumDisplay|exit code' "$L" | tail -10
echo ""
echo "=== last 25 lines before exit ==="
ln=$(grep -anE 'PE process exited|Execution finished' "$L" | tail -1 | cut -d: -f1)
if [ -n "$ln" ]; then sed -n "$((ln-25)),$((ln+2))p" "$L"; fi
