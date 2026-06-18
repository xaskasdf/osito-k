#!/usr/bin/env bash
# Build the OsitoK real-hardware USB bundle on macOS.
#
# Layout:
#   diskNs1: 512 MiB EFI System Partition, FAT32
#   diskNs2: rest of disk, GPT label "osito", OsitoFS v2
#
# This script intentionally validates /dev/disk2 by default because it is
# destructive. Pass a different device only when you have revalidated it.

set -euo pipefail

MODE="full"
if [ "${1:-}" = "--resume-payloads" ]; then
    MODE="payloads"
    shift
fi
DEVICE="${1:-/dev/disk2}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OSITOK_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OK_PORTED="${OK_PORTED:-/Users/pc/ok-ported}"

BOOT_EFI="$OSITOK_ROOT/arch/x86/build/boot.efi"
KERNEL_ELF="$OSITOK_ROOT/arch/x86/build/kernel.elf"
BASE_IMG="$OSITOK_ROOT/nvme_gtav_full.img"
UT99_IMG="$OSITOK_ROOT/nvme_ut99.img"
Q2_IMG="$OSITOK_ROOT/nvme_quake2.img"

OSITOFS_WRITE="$OSITOK_ROOT/tools/ositofs/ositofs-write"
OSITOFS_READ="$OSITOK_ROOT/tools/ositofs/ositofs-read"
OSITOFS_RESIZE="$OSITOK_ROOT/tools/ositofs/ositofs-resize"
OSITOFS_LS="$OSITOK_ROOT/tools/ositofs/ositofs-ls"

ESP_MIB=512
EXPECTED_BYTES_MIN=120000000000
EXPECTED_BYTES_MAX=130000000000

fatal() { printf '[!] %s\n' "$*" >&2; exit 1; }
info()  { printf '[+] %s\n' "$*"; }
warn()  { printf '[!] %s\n' "$*" >&2; }

need_file() {
    [ -f "$1" ] || fatal "missing file: $1"
}

plist_get() {
    /usr/bin/plutil -extract "$2" raw -o - - <<<"$1" 2>/dev/null || true
}

validate_target() {
    local plist ident whole internal proto size block removable physical

    plist="$(diskutil info -plist "$DEVICE")" || fatal "diskutil info failed for $DEVICE"
    ident="$(plist_get "$plist" DeviceIdentifier)"
    whole="$(plist_get "$plist" WholeDisk)"
    internal="$(plist_get "$plist" Internal)"
    proto="$(plist_get "$plist" BusProtocol)"
    size="$(plist_get "$plist" Size)"
    block="$(plist_get "$plist" DeviceBlockSize)"
    removable="$(plist_get "$plist" RemovableMediaOrExternalDevice)"
    physical="$(plist_get "$plist" VirtualOrPhysical)"

    [ "$DEVICE" = "/dev/disk2" ] || fatal "refusing non-default target $DEVICE; edit/run manually after revalidation"
    [ "$ident" = "disk2" ] || fatal "target is $ident, expected disk2"
    [ "$whole" = "true" ] || fatal "$DEVICE is not a whole disk"
    [ "$internal" = "false" ] || fatal "$DEVICE is internal; aborting"
    [ "$proto" = "USB" ] || fatal "$DEVICE BusProtocol is $proto, expected USB"
    [ "$physical" = "Physical" ] || fatal "$DEVICE is not physical media"
    [ "$removable" = "true" ] || fatal "$DEVICE is not reported external/removable"
    [ "$block" = "512" ] || fatal "$DEVICE block size is $block, expected 512"
    [ "$size" -ge "$EXPECTED_BYTES_MIN" ] || fatal "$DEVICE size $size is too small"
    [ "$size" -le "$EXPECTED_BYTES_MAX" ] || fatal "$DEVICE size $size is too large"

    DISK_BYTES="$size"
    DISK_BLOCK_SIZE="$block"

    info "Validated $DEVICE: USB physical external, $DISK_BYTES bytes"
}

require_root() {
    if [ "$(id -u)" != "0" ]; then
        fatal "run this script as root (sudo $0 $DEVICE)"
    fi
}

check_inputs() {
    need_file "$BOOT_EFI"
    need_file "$KERNEL_ELF"
    need_file "$BASE_IMG"
    need_file "$UT99_IMG"
    need_file "$OSITOFS_WRITE"
    need_file "$OSITOFS_READ"
    need_file "$OSITOFS_RESIZE"
    need_file "$OSITOFS_LS"
    command -v sgdisk >/dev/null 2>&1 ||
        fatal "sgdisk is required on macOS for the exact two-partition GPT layout"
}

wait_for_slice() {
    local slice="$1"
    for _ in $(seq 1 20); do
        [ -e "$slice" ] && return 0
        diskutil list "$DEVICE" >/dev/null || true
        sleep 1
    done
    return 1
}

raw_slice_for() {
    printf '/dev/r%s' "$(basename "$1")"
}

create_partitions() {
    info "Destroying existing partition map on $DEVICE"
    diskutil unmountDisk force "$DEVICE" >/dev/null 2>&1 || true

    info "Creating GPT: ESP ${ESP_MIB}MiB + osito data partition"
    sgdisk --zap-all "$DEVICE"
    sgdisk --clear \
        --new=1:2048:+"${ESP_MIB}"M --typecode=1:ef00 --change-name=1:EFI \
        --new=2:0:0              --typecode=2:8300 --change-name=2:osito \
        "$DEVICE"

    diskutil list "$DEVICE"
    diskutil unmountDisk force "$DEVICE" >/dev/null 2>&1 || true

    wait_for_slice "${DEVICE}s1" || fatal "${DEVICE}s1 did not appear"
    wait_for_slice "${DEVICE}s2" || fatal "${DEVICE}s2 did not appear"
}

populate_esp() {
    local esp="${DEVICE}s1"
    local raw_esp
    local mnt

    raw_esp="$(raw_slice_for "$esp")"

    info "Formatting ESP ($esp) as FAT32"
    newfs_msdos -F 32 -v EFI "$raw_esp" >/dev/null

    mnt="$(mktemp -d /tmp/ositok-esp.XXXXXX)"
    mount -t msdos "$esp" "$mnt"
    mkdir -p "$mnt/EFI/BOOT"
    cp "$BOOT_EFI" "$mnt/EFI/BOOT/BOOTX64.EFI"
    cp "$KERNEL_ELF" "$mnt/EFI/BOOT/kernel.elf"
    printf 'FS0:\\EFI\\BOOT\\BOOTX64.EFI\r\n' > "$mnt/startup.nsh"
    sync
    umount "$mnt"
    rmdir "$mnt"
    info "ESP populated"
}

write_one() {
    local image="$1"
    local local_path="$2"
    local stored_name="$3"
    local overwrite="${4:-}"

    [ -f "$local_path" ] || return 0
    if [ "$overwrite" = "--overwrite" ]; then
        "$OSITOFS_WRITE" "$image" "$local_path" --name "$stored_name" --overwrite
    else
        "$OSITOFS_WRITE" "$image" "$local_path" --name "$stored_name"
    fi
}

extract_one() {
    local image="$1"
    local stored="$2"
    local out="$3"
    "$OSITOFS_READ" "$image" "$stored" "$out" >/dev/null
}

copy_base_ositofs() {
    local data="${DEVICE}s2"
    local raw_data

    raw_data="$(raw_slice_for "$data")"

    info "Copying GTA base image into $data"
    dd if="$BASE_IMG" of="$raw_data" bs=4m conv=notrunc
    sync

    info "Expanding OsitoFS superblock to full partition size"
    "$OSITOFS_RESIZE" "$data" --label osito
}

add_payloads_ositofs() {
    local data="$1"
    local tmp gta_src q2_elf q2_pak doom_src ut_dir ut_manifest
    local f rel

    tmp="$(mktemp -d /tmp/ositok-bundle.XXXXXX)"
    trap 'rm -rf "$tmp"' EXIT

    gta_src="$OK_PORTED/GTAV_Source/GTA5.elf"
    [ -s "$gta_src" ] || gta_src=""
    if [ -n "$gta_src" ]; then
        info "Updating GTA5.elf from $gta_src"
        write_one "$data" "$gta_src" "GTA5.elf" --overwrite
    else
        warn "No non-empty GTAV_Source/GTA5.elf found; keeping GTA5.elf from base image"
    fi

    info "Adding Doom payloads if present"
    for doom_src in \
        "$OSITOK_ROOT/doom.elf" \
        "$OSITOK_ROOT/DOOM.WAD" \
        "$OSITOK_ROOT/DOOM2.WAD" \
        "$OSITOK_ROOT/PLUTONIA.WAD" \
        "$OSITOK_ROOT/TNT.WAD"; do
        if [ -f "$doom_src" ]; then
            write_one "$data" "$doom_src" "$(basename "$doom_src")"
        fi
    done

    q2_elf="$OK_PORTED/quake-2-ok/quake2.elf"
    [ -f "$q2_elf" ] || q2_elf="$OSITOK_ROOT/quake2.elf"
    q2_pak="$OK_PORTED/quake-2-ok/build_wasm/baseq2/pak0.pak"
    if [ ! -f "$q2_pak" ]; then
        need_file "$Q2_IMG"
        mkdir -p "$tmp/q2/baseq2"
        extract_one "$Q2_IMG" "baseq2/pak0.pak" "$tmp/q2/baseq2/pak0.pak"
        q2_pak="$tmp/q2/baseq2/pak0.pak"
    fi

    info "Adding Quake2 payloads"
    write_one "$data" "$q2_elf" "quake2.elf"
    write_one "$data" "$q2_pak" "baseq2/pak0.pak"

    info "Extracting UT99 image and writing as one prevalidated batch"
    ut_dir="$tmp/ut99"
    ut_manifest="$tmp/ut99.manifest"
    mkdir -p "$ut_dir"
    "$OSITOFS_READ" "$UT99_IMG" "*" --output-dir "$ut_dir" >/dev/null
    : > "$ut_manifest"
    find "$ut_dir" -type f -print | sort | while IFS= read -r f; do
        rel="${f#$ut_dir/}"
        printf '%s %s\n' "$f" "$rel" >> "$ut_manifest"
    done
    "$OSITOFS_WRITE" "$data" --from-list "$ut_manifest"

    info "Final OsitoFS listing summary"
    "$OSITOFS_LS" "$data" | sed -n '1,80p'
}

populate_ositofs() {
    copy_base_ositofs
    add_payloads_ositofs "${DEVICE}s2"
}

main() {
    require_root
    check_inputs
    validate_target

    if [ "$MODE" = "payloads" ]; then
        warn "Existing OsitoFS on ${DEVICE}s2 will be modified."
        printf "Type 'yes' to continue: "
        read -r confirm
        [ "$confirm" = "yes" ] || fatal "aborted"

        add_payloads_ositofs "${DEVICE}s2"
        sync
        diskutil unmountDisk "$DEVICE" >/dev/null 2>&1 || true
        info "Done. Eject $DEVICE and boot the target machine."
        return 0
    fi

    warn "ALL DATA ON $DEVICE WILL BE DESTROYED."
    printf "Type 'yes' to continue: "
    read -r confirm
    [ "$confirm" = "yes" ] || fatal "aborted"

    create_partitions
    populate_esp
    populate_ositofs
    sync
    diskutil unmountDisk "$DEVICE" >/dev/null 2>&1 || true
    info "Done. Eject $DEVICE and boot the target machine."
}

main "$@"
