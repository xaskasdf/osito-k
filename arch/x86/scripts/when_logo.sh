#!/bin/bash
L=/root/osito-run/serial.log
echo "first UT-Logo-Map line:"; grep -anF 'UT-Logo-Map.unr' "$L" | head -1
echo "menu-ready marker line:"; grep -anE 'RegisterClassEx|GetMessage|PeekMessage' "$L" | head -1
echo "total UT-Logo-Map attempts:"; grep -acF 'UT-Logo-Map.unr' "$L"
echo "input events before throw (CauseInputEvent/KeyEvent):"; grep -anE 'CauseInputEvent|KeyEvent' "$L" | head -3
echo "context 12 lines before first UT-Logo-Map attempt:"
ln=$(grep -anF 'UT-Logo-Map.unr' "$L" | head -1 | cut -d: -f1)
[ -n "$ln" ] && sed -n "$((ln-12)),$((ln-1))p" "$L"
