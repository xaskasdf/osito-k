#!/bin/bash
#
# OsitoK x86-64 — Deploy script
#
# Creates OsitoFS partition, formats it, installs EFI binary,
# and adds GRUB entry.
#
# Usage: sudo ./deploy.sh [--partition-only | --efi-only | --all]
#
# Prerequisites:
#   - Built tools/ositofs/ and arch/x86/build/ositok.efi
#   - Free space on /dev/nvme0n1 for partition 6
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"

NVME_DEV="/dev/nvme0n1"
PART_NUM=6
PART_DEV="${NVME_DEV}p${PART_NUM}"
PART_SIZE="1G"
PART_LABEL="OsitoFS-AI"

MKFS="$ROOT_DIR/tools/ositofs/mkfs.ositofs"
EFI_BIN="$ROOT_DIR/arch/x86/build/ositok.efi"
ESP_DIR="/boot/efi/EFI/ositok"
GRUB_CUSTOM="/etc/grub.d/40_custom"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info()  { echo -e "${GREEN}[+]${NC} $*"; }
warn()  { echo -e "${YELLOW}[!]${NC} $*"; }
error() { echo -e "${RED}[!]${NC} $*"; exit 1; }

check_root() {
    [ "$(id -u)" -eq 0 ] || error "Must run as root (sudo)"
}

check_prereqs() {
    [ -f "$MKFS" ]    || error "mkfs.ositofs not found. Run: make -C tools/ositofs"
    [ -f "$EFI_BIN" ] || error "ositok.efi not found. Run: make -C arch/x86"
    [ -b "$NVME_DEV" ] || error "$NVME_DEV not found"
}

show_disk_state() {
    info "Current partition table for $NVME_DEV:"
    sgdisk -p "$NVME_DEV" 2>/dev/null || fdisk -l "$NVME_DEV"
    echo
}

create_partition() {
    if [ -b "$PART_DEV" ]; then
        warn "$PART_DEV already exists"
        read -p "  Skip partition creation? [Y/n] " answer
        case "$answer" in
            [nN]*) ;;
            *) info "Skipping partition creation"; return 0 ;;
        esac
    fi

    info "Creating partition $PART_NUM ($PART_SIZE) on $NVME_DEV..."
    sgdisk -n "${PART_NUM}:0:+${PART_SIZE}" \
           -t "${PART_NUM}:8300" \
           -c "${PART_NUM}:${PART_LABEL}" \
           "$NVME_DEV"

    # Inform kernel of new partition
    partprobe "$NVME_DEV" 2>/dev/null || true
    sleep 1

    [ -b "$PART_DEV" ] || error "$PART_DEV not found after creation"
    info "Partition created: $PART_DEV"
}

format_partition() {
    info "Formatting $PART_DEV with OsitoFS v2..."
    "$MKFS" "$PART_DEV" --label "$PART_LABEL"
    info "Format complete"
}

install_efi() {
    info "Installing EFI binary..."
    mkdir -p "$ESP_DIR"
    cp "$EFI_BIN" "$ESP_DIR/ositok.efi"
    info "Installed: $ESP_DIR/ositok.efi ($(stat -c%s "$EFI_BIN") bytes)"
}

setup_grub() {
    GRUB_ENTRY='menuentry "OsitoK x86 (bare-metal AI)" {
    insmod chain
    insmod part_gpt
    insmod fat
    chainloader /EFI/ositok/ositok.efi
}'

    if grep -q "OsitoK" "$GRUB_CUSTOM" 2>/dev/null; then
        warn "GRUB entry already exists in $GRUB_CUSTOM"
    else
        info "Adding GRUB entry to $GRUB_CUSTOM..."
        echo "" >> "$GRUB_CUSTOM"
        echo "$GRUB_ENTRY" >> "$GRUB_CUSTOM"
        info "GRUB entry added"
    fi

    info "Regenerating GRUB config..."
    grub-mkconfig -o /boot/grub/grub.cfg 2>&1 | grep -i "ositok\|done" || true
    info "GRUB config updated"
}

do_all() {
    show_disk_state
    create_partition
    format_partition
    install_efi
    setup_grub
    echo
    info "Deployment complete!"
    info "Reboot and select 'OsitoK x86 (bare-metal AI)' from GRUB menu"
}

do_partition() {
    show_disk_state
    create_partition
    format_partition
}

do_efi() {
    install_efi
    setup_grub
}

# ── Main ─────────────────────────────────────────────────────

check_root
check_prereqs

case "${1:---all}" in
    --partition-only) do_partition ;;
    --efi-only)       do_efi ;;
    --all)            do_all ;;
    -h|--help)
        echo "Usage: sudo $0 [--partition-only | --efi-only | --all]"
        echo "  --all             Create partition + format + install EFI + GRUB (default)"
        echo "  --partition-only  Only create and format OsitoFS partition"
        echo "  --efi-only        Only install EFI binary and update GRUB"
        exit 0
        ;;
    *) error "Unknown option: $1" ;;
esac
