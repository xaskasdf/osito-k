#!/bin/bash
# Boot UT99 N times, count how many reach the title without a boot crash.
# Runs inside the WSL 'osito' distro. Kills only its own qemu by PID each round.
N="${1:-4}"
RUN=/mnt/c/Users/xasko/osito-k/arch/x86/scripts/run-wsl-kvm.sh
export XDG_RUNTIME_DIR=/mnt/wslg/runtime-dir DISPLAY=:0 WAYLAND_DISPLAY=wayland-0
export OK_DISPLAY=none OK_MONITOR=1 OK_NVME=/mnt/c/Users/xasko/osito-k/nvme_ut99.img
ok=0
for i in $(seq 1 "$N"); do
    # kill any prior qemu on our monitor port
    p=$(pgrep -f 'qemu-system-x86_64.*55555' | head -1)
    [ -n "$p" ] && kill -9 "$p" 2>/dev/null
    sleep 1
    rm -f /root/osito-run/serial.log
    cd /tmp
    setsid bash "$RUN" >/dev/null 2>&1 &
    # wait up to 90s for boot to reach the title region
    for t in $(seq 1 30); do
        sleep 3
        n=$(wc -l < /root/osito-run/serial.log 2>/dev/null || echo 0)
        [ "$n" -ge 17700 ] && break
    done
    sleep 2
    crashes=$(grep -c 'Process crashed' /root/osito-run/serial.log 2>/dev/null); [ -z "$crashes" ] && crashes=0
    lines=$(wc -l < /root/osito-run/serial.log 2>/dev/null || echo 0)
    if [ "$crashes" = "0" ] && [ "$lines" -ge 17700 ]; then
        echo "boot $i: OK (lines=$lines, crashes=0)"
        ok=$((ok+1))
    else
        echo "boot $i: FAIL (lines=$lines, crashes=$crashes)"
    fi
done
# clean up final qemu
p=$(pgrep -f 'qemu-system-x86_64.*55555' | head -1); [ -n "$p" ] && kill -9 "$p" 2>/dev/null
echo "RESULT: $ok/$N boots reached title without crash"
