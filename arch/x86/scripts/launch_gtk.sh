#!/bin/bash
# Foreground gtk launch (run under a persistent background task) so the user can
# drive the UT99 menu (New Game) while we capture #1 from serial.
for p in $(pgrep -f 'qemu-system-x86_64.*osito-run'); do kill -9 "$p" 2>/dev/null; done
sleep 1
rm -f /root/osito-run/nvme_ut99.img
cd /mnt/c/Users/xasko/osito-k
export OK_DISPLAY=gtk
export OK_MONITOR=1
export OK_NVME=/mnt/c/Users/xasko/osito-k/nvme_ut99.img
exec ./arch/x86/scripts/run-wsl-kvm.sh
