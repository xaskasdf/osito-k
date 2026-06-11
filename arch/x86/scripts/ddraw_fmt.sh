#!/bin/bash
L=/root/osito-run/serial.log
echo "=== SetDisplayMode / bpp / surface format ==="
grep -anaiE 'SetDisplayMode|DisplayMode|bpp|RGB|565|555|PixelFormat|ddpf|surface.*creat|CreateSurface|GetSurfaceDesc|lPitch|Pitch|primary' "$L" | tail -40
echo
echo "=== present / blt / flip / GOP ==="
grep -anaiE 'Flip|Blt|present|GOP|framebuffer|fb_|shadow|copy' "$L" | tail -25
