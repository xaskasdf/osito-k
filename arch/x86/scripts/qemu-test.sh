#!/bin/bash
#
# OsitoK x86-64 — QEMU Test Script
#
# Boots ositok.efi in QEMU with UEFI + e1000e NIC.
# No root required. Uses user-mode networking with UDP port forward.
#
# Usage:
#   ./qemu-test.sh              # Build + boot
#   ./qemu-test.sh --no-build   # Boot only (skip build)
#   ./qemu-test.sh --gtk        # Local GTK window, 2D virtio-vga
#   ./qemu-test.sh --egl-headless # GL virtio-gpu via /dev/dri render node
#   ./qemu-test.sh --venus      # real Vulkan host via Venus + blob resources
#
# Test from another terminal:
#   echo "hola osito" | nc -u localhost 7777
#
# Exit QEMU: Ctrl-A X
#

set -e
set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
X86_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$X86_DIR/build"
EFI_BIN="$BUILD_DIR/boot.efi"
KERN_BIN="${QEMU_KERNEL_BIN:-$BUILD_DIR/kernel.elf}"
ESP_IMG="${QEMU_ESP_IMG:-$BUILD_DIR/esp.img}"

# -- Vulkan Wave 1: virtio-gpu-gl-pci is the default display device so
# the guest can negotiate VIRTIO_GPU_F_VIRGL. `--no-gl` falls back to the
# plain 2D virtio-vga for hosts that lack virglrenderer. --
USE_GL="true"
USE_VENUS="false"
DISPLAY_MODE="auto"
USE_KVM="auto"
NO_BUILD="false"
PID_FILE_DEFAULT="/tmp/qemu-test-osito.pid"
PID_FILE="${QEMU_PID_FILE:-$PID_FILE_DEFAULT}"
DO_KILL="false"
for arg in "$@"; do
    case "$arg" in
        --no-gl)    USE_GL="false" ;;
        --gtk)      USE_GL="false"; DISPLAY_MODE="gtk" ;;
        --egl-headless) USE_GL="true"; DISPLAY_MODE="egl-headless" ;;
        --venus)   USE_GL="true"; USE_VENUS="true"; DISPLAY_MODE="egl-headless" ;;
        --vnc)      DISPLAY_MODE="vnc" ;;
        --no-kvm)   USE_KVM="false" ;;
        --no-build) NO_BUILD="true" ;;
        --kill)     DO_KILL="true" ;;
    esac
done

QEMU_MEM="${QEMU_MEM:-4G}"
QEMU_SMP="${QEMU_SMP:-4}"
QEMU_GPU_HOSTMEM="${QEMU_GPU_HOSTMEM:-512M}"
QEMU_RENDER_NODE="${QEMU_RENDER_NODE:-/dev/dri/renderD128}"
QEMU_GPU_BLOB="${QEMU_GPU_BLOB:-0}"
QEMU_VNC="${QEMU_VNC:-:0}"
QEMU_USB_POINTER="${QEMU_USB_POINTER:-tablet}"
QEMU_UDP_PORT="${QEMU_UDP_PORT:-7777}"
QEMU_TCP_PORT="${QEMU_TCP_PORT:-}"
QEMU_TCP_GUEST_PORT="${QEMU_TCP_GUEST_PORT:-$QEMU_TCP_PORT}"
QEMU_MONITOR_SOCKET="${QEMU_MONITOR_SOCKET:-/tmp/qemu-monitor.sock}"
QEMU_AUDIO_PATH="${QEMU_AUDIO_PATH:-$BUILD_DIR/audio.wav}"
QEMU_BIN="${QEMU_BIN:-qemu-system-x86_64}"
QEMU_NVME_CACHE="${QEMU_NVME_CACHE:-writeback}"
QEMU_EXTRA_ARGS="${QEMU_EXTRA_ARGS:-}"

# --kill: terminate a previously-launched instance by reading the PID file.
# Only kills that exact PID — safe for concurrent qemu-system-x86_64 users.
if [ "$DO_KILL" = "true" ]; then
    if [ -f "$PID_FILE" ]; then
        PID="$(cat "$PID_FILE")"
        if kill -0 "$PID" 2>/dev/null; then
            kill "$PID" && echo "[+] killed qemu PID $PID"
        else
            echo "[+] no running qemu for PID $PID (stale file)"
        fi
        rm -f "$PID_FILE"
    else
        echo "[+] no pid file at $PID_FILE — nothing to kill"
    fi
    exit 0
fi
if [ "$USE_GL" = "true" ]; then
    GPU_BLOB_OPT=""
    VENUS_OPT=""
    if [ "$QEMU_GPU_BLOB" = "1" ] || [ "$USE_VENUS" = "true" ]; then
        GPU_BLOB_OPT=",blob=on"
    fi
    [ "$USE_VENUS" = "true" ] && VENUS_OPT=",venus=on"
    GPU_DEVICE="-device virtio-gpu-gl-pci,hostmem=$QEMU_GPU_HOSTMEM$GPU_BLOB_OPT$VENUS_OPT"
else
    GPU_DEVICE="-device virtio-vga"
fi

# OVMF firmware paths (try common locations)
# Prefer the combined OVMF.fd (works with -bios), then split CODE+VARS (needs pflash)
for f in /usr/share/qemu/OVMF.fd \
         /usr/share/ovmf/OVMF.fd \
         /usr/share/OVMF/OVMF_CODE.fd; do
    [ -f "$f" ] && OVMF="$f" && break
done

# If only 4M split firmware available, use pflash mode
if [ -z "$OVMF" ] && [ -f /usr/share/OVMF/OVMF_CODE_4M.fd ]; then
    OVMF="/usr/share/OVMF/OVMF_CODE_4M.fd"
    OVMF_VARS="/usr/share/OVMF/OVMF_VARS_4M.fd"
    OVMF_PFLASH=1
fi

# macOS: Homebrew QEMU ships edk2 split firmware
if [ -z "$OVMF" ]; then
    QEMU_SHARE="$(qemu-system-x86_64 --version 2>/dev/null | head -1 | sed 's/.*version //')"
    for d in /usr/local/Cellar/qemu/*/share/qemu \
             /opt/homebrew/Cellar/qemu/*/share/qemu \
             /opt/homebrew/share/qemu \
             /usr/local/share/qemu; do
        if [ -f "$d/edk2-x86_64-code.fd" ]; then
            OVMF="$d/edk2-x86_64-code.fd"
            # Use i386-vars (compatible with x86_64) or x86_64-specific if present
            for varfile in "$d/edk2-i386-vars.fd" "$d/edk2-x86_64-vars.fd"; do
                [ -f "$varfile" ] && OVMF_VARS="$varfile" && break
            done
            OVMF_PFLASH=1
            break
        fi
    done
fi

RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
NC='\033[0m'

info()  { echo -e "${GREEN}[+]${NC} $*"; }
error() { echo -e "${RED}[!]${NC} $*"; exit 1; }

# ── Check prerequisites ──────────────────────────────────────

command -v "$QEMU_BIN" >/dev/null || error "QEMU not found: $QEMU_BIN"
[ -n "$OVMF" ] || error "OVMF not found. Run: sudo apt install ovmf"
command -v mtools >/dev/null 2>&1 || command -v mcopy >/dev/null 2>&1 || error "mtools not installed. Run: sudo apt install mtools"
case "$QEMU_USB_POINTER" in
    mouse|tablet) ;;
    *) error "QEMU_USB_POINTER must be 'mouse' or 'tablet'" ;;
esac
if [ "$USE_VENUS" = "true" ]; then
    VENUS_HELP="$("$QEMU_BIN" -display egl-headless,gl=on,rendernode="$QEMU_RENDER_NODE" \
        -device virtio-gpu-gl-pci,help 2>&1 || true)"
    case "$VENUS_HELP" in
        *"venus=<bool>"*) ;;
        *) error "QEMU does not expose virtio-gpu Venus; use QEMU 9.2+ built with virglrenderer" ;;
    esac
fi

# ── Build if needed ───────────────────────────────────────────

if [ "$NO_BUILD" != "true" ]; then
    info "Building boot.efi + kernel.elf..."
    # On macOS, use cross-compiler and macOS gnu-efi paths
    if [ "$(uname)" = "Darwin" ]; then
        GNU_EFI="/private/tmp/gnu-efi"
        make -C "$X86_DIR" -j$(sysctl -n hw.ncpu) \
            CC=x86_64-elf-gcc LD=x86_64-elf-ld OBJCOPY=x86_64-elf-objcopy \
            EFI_INC="$GNU_EFI/inc" EFI_INC_ARCH="$GNU_EFI/inc/x86_64" \
            EFI_LIB="$GNU_EFI/x86_64/lib" \
            EFI_CRT="$GNU_EFI/x86_64/gnuefi/crt0-efi-x86_64.o" \
            EFI_LDS="$GNU_EFI/gnuefi/elf_x86_64_efi.lds" \
            2>&1 | tail -5
    else
        make -C "$X86_DIR" -j$(nproc) 2>&1 | tail -5
    fi
fi

[ -f "$EFI_BIN" ]  || error "boot.efi not found at $EFI_BIN"
[ -f "$KERN_BIN" ] || error "kernel.elf not found at $KERN_BIN"

# ── Create ESP image ─────────────────────────────────────────

info "Creating ESP image..."
dd if=/dev/zero of="$ESP_IMG" bs=1M count=64 status=none
mformat -i "$ESP_IMG" -F ::
mmd -i "$ESP_IMG" ::/EFI
mmd -i "$ESP_IMG" ::/EFI/BOOT
mcopy -i "$ESP_IMG" "$EFI_BIN" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$ESP_IMG" "$KERN_BIN" ::/EFI/BOOT/kernel.elf
# startup.nsh: auto-boot via UEFI Shell (needed when VARS lacks boot order)
printf 'FS0:\\EFI\\BOOT\\BOOTX64.EFI\r\n' > /tmp/startup.nsh
mcopy -i "$ESP_IMG" /tmp/startup.nsh ::/startup.nsh

FSIZE() { stat -c%s "$1" 2>/dev/null || stat -f%z "$1" 2>/dev/null || wc -c < "$1"; }
info "ESP image: boot.efi=$(FSIZE "$EFI_BIN") kernel.elf=$(FSIZE "$KERN_BIN")"

# ── Launch QEMU ──────────────────────────────────────────────

info "Launching QEMU..."
info "  OVMF: $OVMF"
info "  NIC:  igb (Intel 82576, advanced descriptors)"
info "  USB:  xHCI + keyboard + $QEMU_USB_POINTER"
info "  RAM:  $QEMU_MEM"
info "  CPUs: $QEMU_SMP"
info "  Net:  user-mode, UDP :$QEMU_UDP_PORT → guest 10.0.2.15:7777"
info ""
echo -e "${CYAN}  Test: echo \"hola osito\" | nc -u localhost 7777${NC}"
echo -e "${CYAN}  Exit: Ctrl-A X${NC}"
info ""

# Build QEMU firmware args
if [ "${OVMF_PFLASH:-0}" = "1" ]; then
    # Copy VARS to temp (pflash needs writable vars)
    VARS_TMP="$BUILD_DIR/ovmf_vars.fd"
    cp "$OVMF_VARS" "$VARS_TMP"
    BIOS_ARGS="-drive if=pflash,format=raw,readonly=on,file=$OVMF \
               -drive if=pflash,format=raw,file=$VARS_TMP"
else
    BIOS_ARGS="-bios $OVMF"
fi

SERIAL_LOG="${QEMU_SERIAL_LOG:-$BUILD_DIR/serial.log}"
rm -f "$SERIAL_LOG"
info "Serial log: $SERIAL_LOG"
info "  (tail -f $SERIAL_LOG to watch)"

SERIAL_SOCKET="${QEMU_SERIAL_SOCKET:-}"
if [ -n "$SERIAL_SOCKET" ]; then
    rm -f "$SERIAL_SOCKET"
    SERIAL_ARGS=(-chardev "socket,id=osito_serial,path=$SERIAL_SOCKET,server=on,wait=off,logfile=$SERIAL_LOG" \
                 -serial chardev:osito_serial)
    info "Serial input: $SERIAL_SOCKET"
else
    SERIAL_ARGS=(-serial "file:$SERIAL_LOG")
fi

# NVMe disk image (OsitoFS)
NVME_IMG="${QEMU_NVME_IMG:-$BUILD_DIR/nvme.img}"
NVME_ARGS=""
if [ -f "$NVME_IMG" ]; then
    info "NVMe disk: $NVME_IMG ($(FSIZE "$NVME_IMG") bytes, cache=$QEMU_NVME_CACHE)"
    NVME_ARGS="-drive file=$NVME_IMG,format=raw,if=none,id=nvme0,cache=$QEMU_NVME_CACHE -device nvme,serial=deadbeef,drive=nvme0"
fi

# Display: cocoa native window on macOS. On Linux/WSL, --egl-headless is the
# stable GL path for virgl; --gtk is the interactive 2D window path.
if [ "$(uname)" = "Darwin" ]; then
    if [ "$USE_GL" = "true" ]; then
        # cocoa lacks OpenGL on macOS QEMU builds — use SDL with gl=core
        # (gl=on tries GLES 3.0 which macOS system GL doesn't support)
        DISPLAY_ARGS="-display sdl,gl=core"
        info "Display: SDL window (gl=core for virgl)"
    else
        DISPLAY_ARGS="-display cocoa"
        info "Display: native macOS window (cocoa)"
    fi
else
    if [ "$DISPLAY_MODE" = "auto" ]; then
        if [ "$USE_GL" = "true" ] && [ -e "$QEMU_RENDER_NODE" ]; then
            DISPLAY_MODE="egl-headless"
        else
            DISPLAY_MODE="gtk"
        fi
    fi

    case "$DISPLAY_MODE" in
        egl-headless)
            [ -e "$QEMU_RENDER_NODE" ] || error "render node not found: $QEMU_RENDER_NODE"
            DISPLAY_ARGS="-display egl-headless,gl=on,rendernode=$QEMU_RENDER_NODE -vnc $QEMU_VNC"
            info "Display: EGL headless GL (rendernode=$QEMU_RENDER_NODE, VNC=$QEMU_VNC)"
            ;;
        gtk)
            GPU_DEVICE="-device virtio-vga"
            DISPLAY_ARGS="-display gtk,gl=off"
            info "Display: GTK window (2D virtio-vga)"
            ;;
        vnc)
            if [ "$USE_GL" = "true" ]; then
                GPU_DEVICE="-device virtio-vga"
                info "Display: VNC (virgl disabled; VNC is not GL-capable)"
            fi
            DISPLAY_ARGS="-vnc $QEMU_VNC,password=on"
            ;;
        *)
            error "unknown display mode: $DISPLAY_MODE"
            ;;
    esac
fi

ACCEL_ARGS="-accel tcg"
CPU_ARGS="-cpu Nehalem"
if [ "$(uname)" != "Darwin" ] && [ "$USE_KVM" != "false" ] && [ -e /dev/kvm ]; then
    ACCEL_ARGS="-accel kvm"
    CPU_ARGS="-cpu host"
    info "Accel: KVM"
else
    info "Accel: TCG"
fi

NETDEV_ARGS="user,id=net0,hostfwd=udp::${QEMU_UDP_PORT}-:7777"
if [ -n "$QEMU_TCP_PORT" ]; then
    NETDEV_ARGS="${NETDEV_ARGS},hostfwd=tcp::${QEMU_TCP_PORT}-:${QEMU_TCP_GUEST_PORT}"
fi

"$QEMU_BIN" \
    $ACCEL_ARGS \
    $BIOS_ARGS \
    -drive file="$ESP_IMG",format=raw,if=ide \
    $NVME_ARGS \
    -m "$QEMU_MEM" \
    -machine q35 \
    $CPU_ARGS \
    -smp "$QEMU_SMP" \
    -device igb,netdev=net0 \
    -netdev "$NETDEV_ARGS" \
    -device qemu-xhci,id=usb \
    -device usb-kbd,bus=usb.0 \
    -device "usb-$QEMU_USB_POINTER,bus=usb.0" \
    -audiodev wav,id=wav0,path="$QEMU_AUDIO_PATH" \
    -device intel-hda,id=hda0 \
    -device hda-duplex,id=snd0,audiodev=wav0 \
    $GPU_DEVICE \
    $DISPLAY_ARGS \
    "${SERIAL_ARGS[@]}" \
    -monitor unix:"$QEMU_MONITOR_SOCKET",server,nowait \
    $QEMU_EXTRA_ARGS \
    -name osito-test \
    -no-reboot -no-shutdown &

QEMU_PID=$!
echo "$QEMU_PID" > "$PID_FILE"
info "QEMU PID: $QEMU_PID (pidfile: $PID_FILE)"
info "  Stop with: bash $0 --kill"

# On INT/TERM (Ctrl-C or explicit kill of the script), forward to qemu so
# it doesn't outlive the intended session. On natural EXIT (qemu finished
# on its own), just clean up the pidfile — do NOT re-kill qemu, or a
# background-launched script that gets SIGHUP on terminal detach will
# also reap qemu (which is what we DON'T want for `--no-build &` flows).
trap 'rm -f "$PID_FILE"' EXIT
trap 'kill "$QEMU_PID" 2>/dev/null; rm -f "$PID_FILE"; exit 130' INT TERM

wait "$QEMU_PID"
QEMU_RC=$?
exit $QEMU_RC
