#!/bin/bash
# UT99/QEMU debug helper for the Win32 layer. Run inside the `osito` WSL distro.
set -euo pipefail

REPO="${OK_REPO:-/mnt/c/Users/xasko/osito-k}"
STAGE="${OK_STAGE:-/root/osito-run}"
NVME="${OK_NVME:-$REPO/nvme_ut99.img}"
MON_HOST="${OK_MON_HOST:-127.0.0.1}"
MON_PORT="${OK_MON_PORT:-55555}"
SERIAL="$STAGE/serial.log"
RUN_OUT="$STAGE/run.out"
BUILD_OUT="$REPO/arch/x86/build"

usage() {
    cat <<'EOF'
Usage: ut99_debug.sh <command>

Commands:
  launch-gtk   Run UT99 under QEMU/KVM with a visible GTK window and monitor TCP.
  status       Show QEMU process, runner output, serial size, and fault summary.
  snap [name]  Capture QEMU framebuffer to arch/x86/build/<name>.png.
  mon <cmd>    Send one raw QEMU HMP monitor command.
  key <k> [ms] Send a QEMU key, default hold 300 ms.
  fire         Send a left-click press/release. Useful to spawn/enter pawn.
  move-test    Capture, hold Up, capture, then print recent input markers.
  first-fault  Print context around the first real #PF.
  faults       Print first #PF context and recent Win32/UT trace markers.
  input        Print recent Win32 input/capture/focus markers.
  markers      Print recent crash, loader, windowing, and DirectDraw markers.
  tail [n]     Tail the serial log, default 120 lines.
  stop         Kill this helper's QEMU instance.
EOF
}

qemu_pids() {
    pgrep -f 'qemu-system-x86_64 .*osito-run' 2>/dev/null || true
}

cmd_launch_gtk() {
    mkdir -p "$STAGE"
    export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/mnt/wslg/runtime-dir}"
    export DISPLAY="${DISPLAY:-:0}"
    export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
    export GDK_BACKEND="${GDK_BACKEND:-x11}"
    export OK_DISPLAY=gtk
    export OK_MONITOR=1
    export OK_NVME="$NVME"
    exec bash "$REPO/arch/x86/scripts/run-wsl-kvm.sh"
}

cmd_status() {
    echo "== QEMU =="
    if pids="$(qemu_pids)" && [ -n "$pids" ]; then
        ps -fp $pids
    else
        echo "not running"
    fi
    echo
    echo "== Runner =="
    [ -f "$RUN_OUT" ] && tail -40 "$RUN_OUT" || echo "missing: $RUN_OUT"
    echo
    echo "== Serial =="
    if [ -f "$SERIAL" ]; then
        wc -l -c "$SERIAL"
        echo "faults=$(grep -acE '#PF|Page Fault|EXCEPTION|Process crashed|triple|appError|Assertion|Critical' "$SERIAL" || true)"
    else
        echo "missing: $SERIAL"
    fi
}

cmd_snap() {
    local name="${1:-ut99-snap}"
    mkdir -p "$BUILD_OUT" "$STAGE"
    local ppm="$STAGE/$name.ppm"
    local png="$STAGE/$name.png"
    local dst="$BUILD_OUT/$name.png"
    python3 "$REPO/arch/x86/scripts/qemu_mon.py" "$MON_HOST" "$MON_PORT" "screendump $ppm"
    python3 "$REPO/arch/x86/scripts/ppm2png.py" "$ppm" "$png"
    cp -f "$png" "$dst"
    ls -lh "$dst"
}

cmd_mon() {
    [ "$#" -gt 0 ] || { echo "usage: ut99_debug.sh mon <hmp command>"; exit 2; }
    python3 "$REPO/arch/x86/scripts/qemu_mon.py" "$MON_HOST" "$MON_PORT" "$*"
}

cmd_key() {
    local key="${1:-}"
    local hold="${2:-300}"
    [ -n "$key" ] || { echo "usage: ut99_debug.sh key <qemu-key> [hold-ms]"; exit 2; }
    python3 "$REPO/arch/x86/scripts/qemu_mon.py" "$MON_HOST" "$MON_PORT" "sendkey $key $hold"
}

cmd_fire() {
    python3 "$REPO/arch/x86/scripts/qemu_mon.py" "$MON_HOST" "$MON_PORT" \
        "mouse_button 1" "mouse_button 0"
}

cmd_move_test() {
    cmd_snap "ut99-move-before"
    cmd_key up 3000
    cmd_snap "ut99-move-after"
    cmd_input
}

cmd_faults() {
    [ -f "$SERIAL" ] || { echo "missing: $SERIAL"; exit 1; }
    echo "== Fault Summary =="
    grep -anE '#PF|Page Fault|EXCEPTION|Process crashed|triple|appError|Assertion|Critical' "$SERIAL" | head -40 || true
    echo
    echo "== First Fault Context =="
    local first
    first="$(grep -anE '#PF|Page Fault' "$SERIAL" | head -1 | cut -d: -f1 || true)"
    if [ -n "$first" ]; then
        local start=$((first - 120))
        local end=$((first + 80))
        [ "$start" -lt 1 ] && start=1
        sed -n "${start},${end}p" "$SERIAL"
    else
        echo "no #PF found"
    fi
    echo
    echo "== Recent Win32/UT Markers =="
    grep -anE 'CB32|INT2E|GMSTATE|HWBP|FMW|LoadLibrary|GetProcAddress|CreateWindow|PeekMessage|GetMessage|DispatchMessage|WndProc|SetWindow|DDRAW|DirectDraw|WinDrv|UWindowsViewport|Reset|bRealtimeChanged' "$SERIAL" | tail -180 || true
}

cmd_first_fault() {
    local before="${1:-40}"
    local after="${2:-240}"
    [ -f "$SERIAL" ] || { echo "missing: $SERIAL"; exit 1; }
    local first
    first="$(grep -an '!!! EXCEPTION: #PF' "$SERIAL" | head -1 | cut -d: -f1 || true)"
    if [ -z "$first" ]; then
        echo "no #PF found"
        return 0
    fi
    local start=$((first - before))
    local end=$((first + after))
    [ "$start" -lt 1 ] && start=1
    echo "first_fault_line=$first range=${start},${end}"
    sed -n "${start},${end}p" "$SERIAL"
}

cmd_tail() {
    local n="${1:-120}"
    tail -n "$n" "$SERIAL"
}

cmd_markers() {
    [ -f "$SERIAL" ] || { echo "missing: $SERIAL"; exit 1; }
    grep -anE 'NULL-CALL|RET0-DIAG|#PF Page Fault|Process crashed|SEH unhandled|appError|Assertion|Critical|ExitProcess|LoadLibrary|Render\.dll|WinDrv|CreateWindow|DDRAW|DirectDraw|UWindowsViewport|SetCooperative|SetDisplay|PeekMessage|GetMessage|DispatchMessage' "$SERIAL" | tail -200 || true
}

cmd_input() {
    [ -f "$SERIAL" ] || { echo "missing: $SERIAL"; exit 1; }
    grep -anE '\[CIE\]|\[CAP\]|\[KEY-IN\]|\[KEYSTATE\]|\[DISPATCH\]|\[MOUSE-CURSOR\]|\[MOUSE-RECT\]|CAP-MOUSE|CAP-MSG|GetFocus|ShowCursor|ClipCursor|SetCursorPos|SetCapture|ReleaseCapture|GetKeyState|GetAsyncKeyState|WM_MOUSE|WM_KEY' "$SERIAL" | tail -360 || true
}

cmd_stop() {
    local pids
    pids="$(qemu_pids)"
    if [ -n "$pids" ]; then
        kill $pids
        echo "stopped: $pids"
    else
        echo "not running"
    fi
}

case "${1:-}" in
    launch-gtk) shift; cmd_launch_gtk "$@" ;;
    status) shift; cmd_status "$@" ;;
    snap) shift; cmd_snap "$@" ;;
    mon) shift; cmd_mon "$@" ;;
    key) shift; cmd_key "$@" ;;
    fire) shift; cmd_fire "$@" ;;
    move-test) shift; cmd_move_test "$@" ;;
    first-fault) shift; cmd_first_fault "$@" ;;
    faults) shift; cmd_faults "$@" ;;
    input) shift; cmd_input "$@" ;;
    markers) shift; cmd_markers "$@" ;;
    tail) shift; cmd_tail "$@" ;;
    stop) shift; cmd_stop "$@" ;;
    *) usage; exit 2 ;;
esac
