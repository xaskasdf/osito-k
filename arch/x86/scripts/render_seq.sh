#!/bin/bash
L=/root/osito-run/serial.log
echo "=== render/device markers (line: text) ==="
grep -anE 'SoftDrv|SetDisplayMode|SetCooperativeLevel|Setting 0x0|RenderDevice|Viewport|GetAttachedSurface|CreateSurface|Triple buffer|D3DDrv|OpenGl|HoldCount|Failed|Bound to|LoadLibrary.*Drv' "$L" | tail -40
echo
echo "=== first throw_from=0x10903EE4 (funclet rethrow start) line ==="
grep -anF 'thrown_from=0x0x10903EE4' "$L" | head -3
echo "=== earliest critical/error/appError/GError markers after line 17800 ==="
awk 'NR>17800' "$L" | grep -anE 'GErrorHist|GIsCriticalError|appError|Critical|Assert|Failed|HoldCount' | head -20
