#!/bin/bash
for pid in $(pgrep -f qemu-system-x86_64); do echo "killing $pid"; kill -9 "$pid" 2>/dev/null; done
sleep 2
if ss -ltn 2>/dev/null | grep -q 55555; then echo BUSY; else echo FREE; fi
rm -f /root/osito-run/serial.log
echo cleared
