#!/bin/bash
L=/root/osito-run/serial.log
echo "=== all DDRAW + format/caps + GDI device-caps calls, in order (up to first PXDUMP) ==="
end=$(grep -an 'PXDUMP' "$L" | head -1 | cut -d: -f1)
[ -z "$end" ] && end=$(wc -l < "$L")
head -n "$end" "$L" | grep -anaiE 'DDRAW|GetDisplayMode|GetSurfaceDesc|GetPixelFormat|GetCaps|EnumDisplayModes|GetDeviceCaps|DIBSection|CreateDIB|BITMAPINFO|SetDisplayMode|created surface|Lock|RGBQUAD|565|555' | tail -60
