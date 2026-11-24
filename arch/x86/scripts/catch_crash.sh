#!/bin/bash
L=/root/osito-run/serial.log
echo "alive=$(pgrep -fc 'qemu-system-x86_64.*55555') lines=$(wc -l < "$L")"
echo "=== crash/exit/throw/appError markers (last 12) ==="
grep -anaiE 'Process crashed|PE exit|ExitProcess|appError|Critical Error|Unhashed|GPF|#PF|page fault|throw|assert|Failed|GErrorHist|exception' "$L" 2>/dev/null | tail -25
echo
echo "=== last 25 non-empty lines ==="
grep -av '^$' "$L" | tail -25
