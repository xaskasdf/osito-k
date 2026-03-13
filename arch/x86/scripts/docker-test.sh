#!/bin/bash
#
# OsitoK x86-64 — Docker + KVM Test Runner
#
# Runs OsitoK in QEMU with KVM acceleration inside Docker.
# Source tree is bind-mounted — builds happen in-container,
# artifacts are visible on host.
#
# Usage:
#   ./docker-test.sh                    # KVM + virtual devices
#   ./docker-test.sh --passthrough      # + SN740 NVMe via VFIO
#   ./docker-test.sh --build-only       # Build, don't boot
#   ./docker-test.sh --shell            # Drop into container shell
#   ./docker-test.sh --no-build         # Skip build, boot only
#
# The serial log is at arch/x86/build/serial.log
# Tail it from another terminal: tail -f arch/x86/build/serial.log
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
X86_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_DIR="$(cd "$X86_DIR/../.." && pwd)"
DOCKER_DIR="$X86_DIR/docker"

IMAGE_NAME="ositok-dev"
CONTAINER_NAME="ositok-test"

RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
YELLOW='\033[0;33m'
NC='\033[0m'

info()  { echo -e "${GREEN}[+]${NC} $*"; }
warn()  { echo -e "${YELLOW}[!]${NC} $*"; }
error() { echo -e "${RED}[!]${NC} $*"; exit 1; }

# ── Parse args ──────────────────────────────────────────────

MODE="kvm"
DO_BUILD=1
PASSTHROUGH=0

for arg in "$@"; do
    case "$arg" in
        --passthrough) PASSTHROUGH=1 ;;
        --build-only)  MODE="build" ;;
        --shell)       MODE="shell" ;;
        --no-build)    DO_BUILD=0 ;;
        --help|-h)
            echo "Usage: $0 [--passthrough] [--build-only] [--shell] [--no-build]"
            exit 0
            ;;
    esac
done

# ── Check prerequisites ──────────────────────────────────────

[ -e /dev/kvm ] || error "KVM not available. Check BIOS virtualization settings."
command -v docker >/dev/null || error "Docker not installed."

# ── Build Docker image if needed ──────────────────────────────

if ! docker image inspect "$IMAGE_NAME" >/dev/null 2>&1; then
    info "Building Docker image '$IMAGE_NAME'..."
    docker build -t "$IMAGE_NAME" "$DOCKER_DIR"
else
    info "Docker image '$IMAGE_NAME' exists"
fi

# ── Stop any running container ────────────────────────────────

docker rm -f "$CONTAINER_NAME" 2>/dev/null || true

# ── NVMe VFIO passthrough setup ──────────────────────────────

PASSTHROUGH_ARGS=""
SN740_BDF="0000:01:00.0"
SN740_VID="15b7"
SN740_PID="5017"

if [ "$PASSTHROUGH" = "1" ]; then
    info "Setting up NVMe passthrough (SN740 @ $SN740_BDF)..."

    # Check current driver
    CURRENT_DRV=$(basename "$(readlink /sys/bus/pci/devices/$SN740_BDF/driver 2>/dev/null)" 2>/dev/null || echo "none")
    info "  Current driver: $CURRENT_DRV"

    if [ "$CURRENT_DRV" != "vfio-pci" ]; then
        # Load vfio-pci module
        sudo modprobe vfio-pci 2>/dev/null || true

        # Unmount any partitions on nvme0n1
        for part in /dev/nvme0n1p*; do
            if mountpoint -q "$part" 2>/dev/null || findmnt "$part" >/dev/null 2>&1; then
                warn "  Unmounting $part..."
                sudo umount "$part" 2>/dev/null || true
            fi
        done

        # Unbind from current driver
        if [ "$CURRENT_DRV" != "none" ]; then
            info "  Unbinding from $CURRENT_DRV..."
            echo "$SN740_BDF" | sudo tee /sys/bus/pci/devices/$SN740_BDF/driver/unbind >/dev/null 2>&1 || true
            sleep 1
        fi

        # Bind to vfio-pci
        info "  Binding to vfio-pci..."
        echo "$SN740_VID $SN740_PID" | sudo tee /sys/bus/pci/drivers/vfio-pci/new_id >/dev/null 2>&1 || true
        sleep 1

        # Verify
        NEW_DRV=$(basename "$(readlink /sys/bus/pci/devices/$SN740_BDF/driver 2>/dev/null)" 2>/dev/null || echo "none")
        if [ "$NEW_DRV" = "vfio-pci" ]; then
            info "  SN740 bound to vfio-pci"
        else
            error "  Failed to bind SN740 to vfio-pci (current: $NEW_DRV)"
        fi
    else
        info "  Already bound to vfio-pci"
    fi

    # Find VFIO group
    IOMMU_GROUP=$(basename "$(readlink /sys/bus/pci/devices/$SN740_BDF/iommu_group)")
    VFIO_DEV="/dev/vfio/$IOMMU_GROUP"
    [ -e "$VFIO_DEV" ] || error "VFIO device $VFIO_DEV not found"
    info "  VFIO group: $IOMMU_GROUP ($VFIO_DEV)"

    PASSTHROUGH_ARGS="--device=$VFIO_DEV --device=/dev/vfio/vfio"
fi

# ── Build QEMU command ────────────────────────────────────────

# Find OVMF inside container
OVMF_PATH="/usr/share/qemu/OVMF.fd"

# NVMe image for OsitoFS (when not using passthrough)
NVME_IMG="$X86_DIR/build/nvme.img"
NVME_QEMU_ARGS=""
if [ "$PASSTHROUGH" = "0" ] && [ -f "$NVME_IMG" ]; then
    NVME_QEMU_ARGS="-drive file=/osito-k/arch/x86/build/nvme.img,format=raw,if=none,id=nvme0 -device nvme,serial=deadbeef,drive=nvme0"
fi

# VFIO NVMe args
VFIO_QEMU_ARGS=""
if [ "$PASSTHROUGH" = "1" ]; then
    VFIO_QEMU_ARGS="-device vfio-pci,host=${SN740_BDF#0000:}"
fi

SERIAL_LOG="/osito-k/arch/x86/build/serial.log"

QEMU_CMD="qemu-system-x86_64 \
    -enable-kvm \
    -cpu host \
    -bios $OVMF_PATH \
    -drive file=/osito-k/arch/x86/build/esp.img,format=raw,if=ide \
    $NVME_QEMU_ARGS \
    $VFIO_QEMU_ARGS \
    -m 4G \
    -machine q35 \
    -smp 4 \
    -device e1000e,netdev=net0 \
    -netdev user,id=net0,hostfwd=udp::7777-:7777 \
    -device qemu-xhci,id=usb \
    -device usb-kbd,bus=usb.0 \
    -device usb-mouse,bus=usb.0 \
    -display none \
    -serial file:$SERIAL_LOG \
    -monitor unix:/tmp/qemu-monitor.sock,server,nowait \
    -no-reboot"

# ── Common docker args ────────────────────────────────────────

DOCKER_ARGS="--rm --name $CONTAINER_NAME \
    --device /dev/kvm \
    $PASSTHROUGH_ARGS \
    -v $REPO_DIR:/osito-k \
    -w /osito-k"

# ── Execute based on mode ────────────────────────────────────

build_cmd="make -C /osito-k/arch/x86 -j\$(nproc)"

create_esp_cmd="
    dd if=/dev/zero of=/osito-k/arch/x86/build/esp.img bs=1M count=64 status=none && \
    mformat -i /osito-k/arch/x86/build/esp.img -F :: && \
    mmd -i /osito-k/arch/x86/build/esp.img ::/EFI && \
    mmd -i /osito-k/arch/x86/build/esp.img ::/EFI/BOOT && \
    mcopy -i /osito-k/arch/x86/build/esp.img /osito-k/arch/x86/build/boot.efi ::/EFI/BOOT/BOOTX64.EFI && \
    mcopy -i /osito-k/arch/x86/build/esp.img /osito-k/arch/x86/build/kernel.elf ::/EFI/BOOT/kernel.elf"

case "$MODE" in
    shell)
        info "Dropping into container shell..."
        info "  Build:  make -C arch/x86 -j\$(nproc)"
        info "  Boot:   (run the QEMU command below)"
        docker run -it $DOCKER_ARGS "$IMAGE_NAME" bash
        exit 0
        ;;
    build)
        info "Building inside container..."
        docker run $DOCKER_ARGS "$IMAGE_NAME" bash -c "$build_cmd"
        info "Build complete: arch/x86/build/kernel.elf"
        ;;
    kvm)
        if [ "$DO_BUILD" = "1" ]; then
            info "Building inside container..."
            docker run $DOCKER_ARGS "$IMAGE_NAME" bash -c "$build_cmd" || error "Build failed"
        fi

        info "Creating ESP image..."
        docker run $DOCKER_ARGS "$IMAGE_NAME" bash -c "$create_esp_cmd" || error "ESP creation failed"

        EFI_SZ=$(stat -c%s "$X86_DIR/build/boot.efi" 2>/dev/null || echo "?")
        KERN_SZ=$(stat -c%s "$X86_DIR/build/kernel.elf" 2>/dev/null || echo "?")
        info "ESP image: boot.efi=$EFI_SZ kernel.elf=$KERN_SZ"

        rm -f "$X86_DIR/build/serial.log"

        info "Launching QEMU with KVM..."
        info "  CPU:     host (AVX2, SSE4.2, FMA)"
        info "  Memory:  4 GB"
        info "  SMP:     4 cores"
        info "  NIC:     e1000e (user-mode)"
        if [ "$PASSTHROUGH" = "1" ]; then
            info "  NVMe:    SN740 passthrough (VFIO)"
        elif [ -f "$NVME_IMG" ]; then
            info "  NVMe:    virtual ($NVME_IMG)"
        fi
        info ""
        echo -e "${CYAN}  Serial:  tail -f arch/x86/build/serial.log${NC}"
        echo -e "${CYAN}  Test:    echo 'hola osito' | nc -u localhost 7777${NC}"
        echo -e "${CYAN}  Stop:    docker stop $CONTAINER_NAME${NC}"
        info ""

        # Use -it only if attached to a terminal
        TTY_FLAG=""
        [ -t 0 ] && TTY_FLAG="-it"

        docker run $TTY_FLAG $DOCKER_ARGS \
            -p 7777:7777/udp \
            "$IMAGE_NAME" \
            bash -c "rm -f $SERIAL_LOG && $QEMU_CMD"
        ;;
esac

# ── Restore NVMe driver if passthrough was used ───────────────

if [ "$PASSTHROUGH" = "1" ]; then
    warn "Restoring SN740 to nvme driver..."
    echo "$SN740_BDF" | sudo tee /sys/bus/pci/devices/$SN740_BDF/driver/unbind >/dev/null 2>&1 || true
    echo "$SN740_BDF" | sudo tee /sys/bus/pci/drivers/nvme/bind >/dev/null 2>&1 || true
    sleep 2
    NEW_DRV=$(basename "$(readlink /sys/bus/pci/devices/$SN740_BDF/driver 2>/dev/null)" 2>/dev/null || echo "none")
    info "SN740 driver restored: $NEW_DRV"
fi
