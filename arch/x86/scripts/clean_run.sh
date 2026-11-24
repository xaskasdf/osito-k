#!/bin/bash
# Clean isolated headless run of the cmdline-map kernel for #1 repro.
set -e
# Force a fresh nvme stage from the pristine source (drop possibly-clobbered staged copy).
rm -f /root/osito-run/nvme_ut99.img
cd /mnt/c/Users/xasko/osito-k
export OK_DISPLAY=none
export OK_MONITOR=0
export OK_NVME=/mnt/c/Users/xasko/osito-k/nvme_ut99.img
nohup ./arch/x86/scripts/run-wsl-kvm.sh > /root/run.out 2>&1 &
echo "LAUNCHED wrapper pid=$!"
sleep 2
echo "=== run.out so far ==="
cat /root/run.out 2>/dev/null | tail -15
