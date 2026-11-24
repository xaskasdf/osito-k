#!/bin/bash
# Kill my prior qemu, launch headless + monitor for console-driven #1 repro.
for p in $(pgrep -f 'qemu-system-x86_64.*osito-run'); do kill -9 "$p" 2>/dev/null; done
sleep 1
rm -f /root/osito-run/nvme_ut99.img
cd /mnt/c/Users/xasko/osito-k
export OK_DISPLAY=none
export OK_MONITOR=1
export OK_NVME=/mnt/c/Users/xasko/osito-k/nvme_ut99.img
nohup ./arch/x86/scripts/run-wsl-kvm.sh > /root/run.out 2>&1 &
echo "LAUNCHED wrapper pid=$!"
