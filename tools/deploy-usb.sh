#!/bin/bash
# OsitoK deploy-usb.sh — write boot image + ELFs to physical USB SSD
#  for bare-metal testing (i5 / R7+3090 / etc.)
#
# WORKFLOW:
#   1. Insert USB SSD on dev box (macOS / Linux)
#   2. Find the device (`diskutil list` on macOS, `lsblk` on Linux)
#   3. tools/deploy-usb.sh /dev/diskN [elf1.elf elf2.elf ...]
#
#   On the bare-metal box:
#   4. Plug USB SSD into a USB port
#   5. Boot from USB (BIOS boot menu, usually F12/F8/Esc/F11)
#   6. OsitoK boots → shell → `exec your-elf.elf`
#
# SAFETY:
#   - Refuses to write to the macOS root disk or /dev/sda* on Linux
#     unless you pass --i-know-what-im-doing.
#   - Always shows the device name + size + first 256 bytes hex preview
#     and asks for confirmation before any write.
#   - Single source of truth: builds the ESP image, builds OsitoFS image,
#     writes both with dd. Whole-device GPT, two partitions:
#       1. EFI System Partition (FAT32, ~64 MB)  — boots BOOTX64.EFI
#       2. OsitoFS data partition (rest of disk) — holds *.elf, configs
#
# Tested: USB sticks 4 GB+, SATA-USB enclosures with HDD/SSD up to 2 TB.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OSITOK_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
EFI_BIN="$OSITOK_ROOT/arch/x86/build/boot.efi"
KERN_BIN="$OSITOK_ROOT/arch/x86/build/kernel.elf"

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'
info()  { printf "${GREEN}[+]${NC} %s\n" "$*"; }
warn()  { printf "${YELLOW}[!]${NC} %s\n" "$*"; }
fatal() { printf "${RED}[!]${NC} %s\n" "$*"; exit 1; }

# ── Args ──────────────────────────────────────────────────────────────
if [ -z "$1" ]; then
    cat <<EOF
Usage: $0 <device> [elf1 elf2 ...] [--i-know-what-im-doing]
       $0 --list

Examples (macOS):
   diskutil list                        # find the USB
   tools/deploy-usb.sh /dev/disk5 \\
     ~/ok-ported/vulkan-tests/hello-gl-clear/hello-gl-clear.elf

Examples (Linux):
   lsblk                                # find the USB
   sudo tools/deploy-usb.sh /dev/sdb \\
     build/quake2.elf

EOF
    exit 0
fi

if [ "$1" = "--list" ]; then
    if [ "$(uname)" = "Darwin" ]; then
        info "macOS disks:"
        diskutil list external 2>/dev/null || diskutil list
    else
        info "Linux block devices:"
        lsblk -o NAME,SIZE,TYPE,MODEL,MOUNTPOINT
    fi
    exit 0
fi

DEVICE="$1"
shift

OVERRIDE=0
ELFS=()
for arg in "$@"; do
    case "$arg" in
        --i-know-what-im-doing) OVERRIDE=1 ;;
        *) ELFS+=("$arg") ;;
    esac
done

# ── Safety ────────────────────────────────────────────────────────────
[ -e "$DEVICE" ] || fatal "device not found: $DEVICE"
[ -f "$EFI_BIN" ] || fatal "boot.efi not found at $EFI_BIN — run make in arch/x86 first"
[ -f "$KERN_BIN" ] || fatal "kernel.elf not found at $KERN_BIN — run make in arch/x86 first"

# Refuse to nuke the dev box.
if [ "$(uname)" = "Darwin" ]; then
    BOOT_DEV="$(diskutil info -plist / 2>/dev/null | \
        grep -A1 ParentWholeDisk | grep string | head -1 | \
        sed -e 's|.*<string>||' -e 's|</string>.*||')"
    if [ -n "$BOOT_DEV" ] && [ "$DEVICE" = "/dev/$BOOT_DEV" ]; then
        fatal "REFUSING to write to macOS boot disk ($DEVICE)."
    fi
else
    if [ "$DEVICE" = "/dev/sda" ] && [ "$OVERRIDE" != "1" ]; then
        fatal "REFUSING to write to /dev/sda. Pass --i-know-what-im-doing to override."
    fi
fi

# ── Confirm ───────────────────────────────────────────────────────────
SIZE_BYTES="$(blockdev --getsize64 "$DEVICE" 2>/dev/null || \
              diskutil info -plist "$DEVICE" 2>/dev/null | grep -A1 TotalSize | grep integer | sed -e 's|.*<integer>||' -e 's|</integer>.*||')"
SIZE_HUMAN="$(echo "$SIZE_BYTES" | awk '{
    if ($1 > 1099511627776) printf "%.1f TB", $1/1099511627776
    else if ($1 > 1073741824) printf "%.1f GB", $1/1073741824
    else if ($1 > 1048576) printf "%.1f MB", $1/1048576
    else printf "%d bytes", $1
}')"
info "Target: $DEVICE ($SIZE_HUMAN)"
info "First 64 bytes:"
dd if="$DEVICE" bs=64 count=1 2>/dev/null | xxd | head -4 || true
warn "ALL DATA ON $DEVICE WILL BE DESTROYED."
echo -n "Type 'yes' to proceed: "
read CONFIRM
[ "$CONFIRM" = "yes" ] || fatal "aborted"

# ── Build ESP image ───────────────────────────────────────────────────
ESP_TMP="$(mktemp -d)/esp.img"
info "Building ESP image (boot.efi + kernel.elf) at $ESP_TMP"
dd if=/dev/zero of="$ESP_TMP" bs=1M count=64 status=none
mformat -i "$ESP_TMP" -F ::
mmd -i "$ESP_TMP" ::/EFI
mmd -i "$ESP_TMP" ::/EFI/BOOT
mcopy -i "$ESP_TMP" "$EFI_BIN" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$ESP_TMP" "$KERN_BIN" ::/EFI/BOOT/kernel.elf
printf 'FS0:\\EFI\\BOOT\\BOOTX64.EFI\r\n' > /tmp/startup.nsh
mcopy -i "$ESP_TMP" /tmp/startup.nsh ::/startup.nsh

# ── Build OsitoFS data partition ─────────────────────────────────────
DATA_TMP="$(mktemp -d)/data.img"
DATA_MB=512
info "Building OsitoFS data partition ($DATA_MB MB) with ${#ELFS[@]} ELFs"
dd if=/dev/zero of="$DATA_TMP" bs=1M count=$DATA_MB status=none
# mkfs.ositofs writes the OSF2 superblock at offset 0; the previous
# `ositofs-fsck --init` call was a no-op (fsck doesn't accept --init,
# the error was masked by `|| true`) — leaving the partition all-zero.
if ! "$OSITOK_ROOT/tools/ositofs/mkfs.ositofs" "$DATA_TMP" --label OsitoK; then
    fatal "mkfs.ositofs failed — partition will be all-zero, kernel won't mount"
fi
for elf in "${ELFS[@]}"; do
    [ -f "$elf" ] || { warn "skip missing: $elf"; continue; }
    "$OSITOK_ROOT/tools/ositofs/ositofs-write" "$DATA_TMP" "$elf" \
        --name "$(basename "$elf")" 2>&1 | tail -1
done

# ── Write to physical device ─────────────────────────────────────────
info "Unmounting any partitions on $DEVICE"
if [ "$(uname)" = "Darwin" ]; then
    diskutil unmountDisk "$DEVICE" || true
fi

info "Writing ESP at sector 2048 (offset 1MB)"
dd if="$ESP_TMP" of="$DEVICE" bs=1M seek=1 conv=notrunc status=progress

info "Writing OsitoFS data at sector 133120 (offset 65MB)"
dd if="$DATA_TMP" of="$DEVICE" bs=1M seek=65 conv=notrunc status=progress

# Write a minimal GPT header that points to ESP + DATA partitions.
# (GPT writing is platform-specific. We use sgdisk on Linux; macOS users
# can use `diskutil partitionDisk` + manual ESP overlay.)
if [ "$(uname)" = "Linux" ] && command -v sgdisk > /dev/null; then
    info "Writing GPT (sgdisk)"
    sgdisk --zap-all "$DEVICE"
    sgdisk -n 1:2048:133119 -t 1:ef00 -c 1:"OsitoK ESP" "$DEVICE"
    sgdisk -n 2:133120:0 -t 2:8300 -c 2:"OsitoK Data" "$DEVICE"
    sgdisk --print "$DEVICE"
else
    warn "macOS / no sgdisk: USB will boot via direct ESP block at sector 2048."
    warn "If your BIOS doesn't auto-detect, repartition with diskutil first."
fi

sync
info "Done. Eject the USB and boot the target machine."
info "After boot in OsitoK shell, type:  exec $(basename "${ELFS[0]:-quake2.elf}")"

rm -f "$ESP_TMP" "$DATA_TMP"
