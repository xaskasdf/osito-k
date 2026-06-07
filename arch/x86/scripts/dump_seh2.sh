#!/bin/bash
L=/root/osito-run/serial.log
echo "lines=$(wc -l < "$L") alive=$(pgrep -fc 'qemu-system-x86_64.*55555')"
echo "=== guard messages (should fire on bad ptr) ==="
grep -acE 'not mapped|bad TryBlockMap|giving up' "$L"
echo "=== EXC32 / kernel #PF / dispatch markers (tail) ==="
grep -anaiE 'EXC32 vec=14|dispatch code=|frame 0 @|not mapped|RIP = 0x0xFFFF8000|CR2 = 0x|Process crashed' "$L" | tail -22
echo "=== full 2nd (kernel) #PF block ==="
ln=$(grep -anE 'RIP = 0x0xFFFF8000021' "$L" | tail -1 | cut -d: -f1)
[ -n "$ln" ] && sed -n "$((ln-8)),$((ln+12))p" "$L" | grep -aiE 'RIP|CR2|RBX|RCX|RDX|flags|dispatch|frame|not mapped'
