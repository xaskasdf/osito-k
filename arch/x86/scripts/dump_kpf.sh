#!/bin/bash
L=/root/osito-run/serial.log
ln=$(grep -anE 'RIP = 0x0xFFFF8000020B17D8' "$L" | tail -1 | cut -d: -f1)
if [ -z "$ln" ]; then
  ln=$(grep -anE '!!! EXCEPTION: #PF' "$L" | tail -1 | cut -d: -f1)
fi
echo "crash at line $ln"
sed -n "$((ln-2)),$((ln+22))p" "$L" | grep -aiE 'RIP|CR2|RAX|RBX|RCX|RDX|RSI|RDI|RBP|Code @|flags|Process crashed|jmpbuf'
echo "=== PEFIX count + last ==="
grep -ac 'PEFIX' "$L"
grep -anE 'PEFIX' "$L" | tail -3
