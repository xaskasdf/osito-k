#!/bin/bash
# Clean relaunch for B8 router validation. Kills qemu by a pattern that does
# NOT match this script's own command line (avoids pkill self-kill).
pkill -9 -f 'accel kvm' 2>/dev/null
sleep 2
if pgrep -f 'accel kvm' >/dev/null 2>&1; then echo "WARN: qemu still alive"; else echo "qemu clean"; fi
export DISPLAY=:0
OK_DISPLAY=none OK_MONITOR=1 OK_NVME=/mnt/c/Users/xasko/osito-k/nvme_ut99.img \
  nohup bash /mnt/c/Users/xasko/osito-k/arch/x86/scripts/run-wsl-kvm.sh > /root/osito-run/run.out 2>&1 &
echo "launched pid=$!"
sleep 6
echo "=== serial size after 6s ==="
stat -c%s /root/osito-run/serial.log 2>/dev/null
echo "=== qemu running? ==="
pgrep -af 'accel kvm' | head -1
