#!/bin/bash
# OsitoK deploy.sh — one-shot ELF deploy + QEMU reboot.
#
# Usage:
#   tools/deploy.sh <elf-path>            # write to NVMe, restart QEMU --no-gl
#   tools/deploy.sh <elf-path> --gl       # restart with virgl
#   tools/deploy.sh <elf-path> --auto     # auto-exec on boot (no shell needed)
#   tools/deploy.sh <elf-path> --tail     # tail serial.log after launch
#
# Examples:
#   tools/deploy.sh ~/ok-ported/vulkan-tests/hello-gl-clear/hello-gl-clear.elf
#   tools/deploy.sh build/quake2.elf --gl --tail
#
# Removes the old NVMe entry, writes new, kills running QEMU, relaunches.

set -e
set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OSITOK_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
NVME_IMG="$OSITOK_ROOT/arch/x86/build/nvme.img"
QEMU_SCRIPT="$OSITOK_ROOT/arch/x86/scripts/qemu-test.sh"
SERIAL_LOG="$OSITOK_ROOT/arch/x86/build/serial.log"

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'
info()  { printf "${GREEN}[+]${NC} %s\n" "$*"; }
warn()  { printf "${YELLOW}[!]${NC} %s\n" "$*"; }
fatal() { printf "${RED}[!]${NC} %s\n" "$*"; exit 1; }

if [ -z "$1" ]; then
    echo "Usage: $0 <elf-path> [--gl|--no-gl] [--auto] [--tail]"
    exit 1
fi

ELF="$1"
shift
[ -f "$ELF" ] || fatal "ELF not found: $ELF"

GL_FLAG="--no-gl"
AUTO=0
TAIL=0
for arg in "$@"; do
    case "$arg" in
        --gl)    GL_FLAG="" ;;
        --no-gl) GL_FLAG="--no-gl" ;;
        --auto)  AUTO=1 ;;
        --tail)  TAIL=1 ;;
        *) warn "ignoring unknown arg: $arg" ;;
    esac
done

ELF_BASENAME="$(basename "$ELF")"
info "ELF:    $ELF ($(/usr/bin/stat -f%z "$ELF" 2>/dev/null || stat -c%s "$ELF") bytes)"
info "NVMe:   $NVME_IMG"

# 1. Kill any running QEMU
"$QEMU_SCRIPT" --kill > /dev/null 2>&1 || true

# 2. Copy-on-write replacement (also creates the file on first deploy)
"$OSITOK_ROOT/tools/ositofs/ositofs-write" "$NVME_IMG" "$ELF" \
    --name "$ELF_BASENAME" --overwrite 2>&1 | tail -3

# 3. Optional: write a startup script that auto-execs the ELF
if [ "$AUTO" = "1" ]; then
    info "Auto-exec: writing /startup.sh that runs '$ELF_BASENAME'"
    TMP=$(mktemp /tmp/osito-startup.XXXXXX)
    echo "exec $ELF_BASENAME" > "$TMP"
    "$OSITOK_ROOT/tools/ositofs/ositofs-write" "$NVME_IMG" "$TMP" \
        --name "startup.sh" --overwrite 2>&1 | tail -1
    rm -f "$TMP"
fi

# 4. Truncate serial log so tail sees fresh content
> "$SERIAL_LOG"

# 5. Launch QEMU in background, detach from terminal so it survives logout
info "Launching QEMU $GL_FLAG..."
nohup bash "$QEMU_SCRIPT" --no-build $GL_FLAG \
    > /tmp/osito-qemu-deploy.log 2>&1 &
disown

sleep 2
if [ -f /tmp/qemu-test-osito.pid ]; then
    info "QEMU PID: $(cat /tmp/qemu-test-osito.pid)"
fi

info "Run inside OsitoK shell:  exec $ELF_BASENAME"
info "Or: bash $0 ... --auto    (auto-exec on boot)"

# 6. Tail serial log if requested
if [ "$TAIL" = "1" ]; then
    info "Tailing $SERIAL_LOG (Ctrl-C to stop tail; QEMU keeps running):"
    tail -f "$SERIAL_LOG"
fi
