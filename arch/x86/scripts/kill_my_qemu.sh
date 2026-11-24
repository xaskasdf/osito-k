#!/bin/bash
# Kill only qemu instances staged under /root/osito-run (mine), by matching the cmdline.
for p in $(pgrep -f 'qemu-system-x86_64.*osito-run'); do
  echo "killing my qemu pid=$p"
  kill -9 "$p" 2>/dev/null
done
sleep 1
echo "remaining osito-run qemu: $(pgrep -fc 'qemu-system-x86_64.*osito-run')"
