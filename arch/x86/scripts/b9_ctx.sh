#!/bin/bash
L=/root/osito-run/serial.log
echo "=== context around first fault (33850-33936) ==="
sed -n '33850,33936p' "$L"
echo ""
echo "=== any unmap/evict/reclaim/free of 0x10xxxxxx near the crash ==="
grep -anE -i 'unmap|evict|reclaim|uncommit|pe_free|VirtualFree|NtFreeVirtualMemory|teardown|tore down|address space' "$L" | tail -25
