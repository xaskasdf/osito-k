#!/bin/bash
# List live qemu processes (PID, age in seconds, monitor port) without killing anything.
echo "=== live qemu-system processes ==="
ps -eo pid,etimes,cmd | grep '[q]emu-system' | while read -r pid age rest; do
    port=$(echo "$rest" | grep -oE 'tcp:127.0.0.1:[0-9]+' | head -1)
    echo "PID=$pid age=${age}s monitor=$port"
done
echo "=== port 55555 holder ==="
ss -ltnp 2>/dev/null | grep ':55555' || echo "(none / ss unavailable)"
