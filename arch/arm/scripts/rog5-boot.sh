#!/bin/bash
#
# rog5-boot.sh -- Build Osito-K for the ROG Phone 5 and optionally flash it
#
# The packing inputs (sm8350-boot shim, rog5-patched.dtb, mkbootimg.py, UEFI
# payload) live in phone_breaker2 on the Mac, which also has the aarch64 gcc
# (Homebrew aarch64-elf-gcc; clang can't build this tree, it has no
# __builtin_setjmp on aarch64). So: ship the tree to the Mac, build and pack
# there, bring boot.img back, flash it from here. See arch/arm/BUILDING-ROG5.md.
#
# Usage (Git Bash / MSYS2 on Windows, or any Unix shell):
#   arch/arm/scripts/rog5-boot.sh                 # build + pack -> arch/arm/build/rog5/osito-boot-rog5.img
#   arch/arm/scripts/rog5-boot.sh --flash         # ... then fastboot flash boot_<slot> + reboot
#   arch/arm/scripts/rog5-boot.sh --usb           # ... then send the kernel to the running Osito-K
#                                                 #     over its USB console (`kload`), no flashing
#   arch/arm/scripts/rog5-boot.sh --virt-check    # also build PLATFORM=virt and boot it in local QEMU
#
# --usb needs a kernel with `kload` already running (the flashed one is enough:
# each loaded kernel can load the next). It leaves boot_a untouched, so a
# reboot goes back to the flashed kernel.
#
# It ships the working tree (arch/arm, gui, include/common), uncommitted
# changes included.
#
# Environment:
#   OK_MAC       Mac with the toolchain + phone_breaker2 (default cortessamuel@192.168.1.4)
#   OK_MAC_JUMP  SSH jump host (default xasko@kerberus; set it empty to connect directly).
#                Port 22 on the Mac times out from the Windows dev box, not from kerberus.
#   OK_MAC_DIR   build dir on the Mac, under ~ (default osito-arm-build)
#   OK_PB        phone_breaker2 on the Mac, under ~ (default phone_breaker2)
#   FASTBOOT     fastboot binary (default: PATH, then /c/platform-tools/fastboot.exe)
#   QEMU         qemu-system-aarch64 (default: PATH, then msys2's /c/msys64/mingw64/bin)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ARCH_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO="$(cd "$ARCH_DIR/../.." && pwd)"
OUT_DIR="$ARCH_DIR/build/rog5"
OUT_REL="arch/arm/build/rog5/osito-boot-rog5.img"

MAC="${OK_MAC:-cortessamuel@192.168.1.4}"
JUMP="${OK_MAC_JUMP-xasko@kerberus}"
MAC_DIR="${OK_MAC_DIR:-osito-arm-build}"
PB="${OK_PB:-phone_breaker2}"

info() { printf '\033[1;36m[rog5]\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[rog5] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

FLASH=0; VIRT=0; USB=0
for a in "$@"; do
    case "$a" in
        --flash)        FLASH=1 ;;
        --usb)          USB=1 ;;
        --virt-check)   VIRT=1 ;;
        -h|--help)      sed -n '3,/^set -e/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *)              die "unknown option: $a (see --help)" ;;
    esac
done
[ "$FLASH" = 1 ] && [ "$USB" = 1 ] && die "--flash and --usb are alternatives"

# OpenSSH 10 warns on every connection without a post-quantum kex; the option
# that silences it doesn't exist in older clients.
SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=15)
QUIET_KEX=()
ssh -G -o WarnWeakCrypto=no localhost >/dev/null 2>&1 && QUIET_KEX=(-o WarnWeakCrypto=no)
SSH_OPTS+=("${QUIET_KEX[@]}")
[ -n "$JUMP" ] && SSH_OPTS+=(-o "ProxyCommand=ssh ${QUIET_KEX[*]} -o BatchMode=yes -W %h:%p $JUMP")
mac() { ssh "${SSH_OPTS[@]}" "$MAC" "$@"; }

cd "$REPO"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# --- Ship the tree ----------------------------------------------------------
# Tracked + untracked (not ignored) files that still exist on disk.
git ls-files -co --exclude-standard -- arch/arm gui include/common \
    | grep -v '^arch/arm/build/' \
    | while IFS= read -r f; do [ -f "$f" ] && printf '%s\n' "$f"; done > "$TMP/files"
info "Sending $(wc -l < "$TMP/files") files to $MAC:~/$MAC_DIR/src..."
tar czf - -T "$TMP/files" | mac "rm -rf ~/$MAC_DIR/src && mkdir -p ~/$MAC_DIR/src && tar xzf - -C ~/$MAC_DIR/src"

# --- Build + pack on the Mac ------------------------------------------------
# Same layout and mkbootimg arguments as phone_breaker2/repack_boot.sh, with
# Osito-K instead of Linux at 1.5 MB.
info "Building PLATFORM=sm8350$([ "$VIRT" = 1 ] && echo ' + virt') and packing boot.img..."
REMOTE_SHA="$(mac "MAC_DIR=$MAC_DIR PB=$PB VIRT=$VIRT bash -s" <<'EOF'
set -e
export PATH=/opt/homebrew/bin:/usr/local/bin:$PATH
D=$HOME/$MAC_DIR; P=$HOME/$PB
build() {
    cd "$D/src/arch/arm"
    rm -rf build
    if ! make -j8 PLATFORM=$1 CROSS=aarch64-elf- > "$D/build-$1.log" 2>&1; then
        grep -E 'error|undefined' "$D/build-$1.log" | head -20 >&2
        echo "build $1 failed, full log: ~/$MAC_DIR/build-$1.log" >&2
        exit 1
    fi
    cp build/Image "$D/Image-$1"
}
build sm8350
[ "$VIRT" = 1 ] && build virt
rm -rf "$D/pack" && mkdir -p "$D/pack" && cd "$D/pack"
dd if=/dev/zero of=combined_boot.bin bs=1M count=45 status=none
dd if="$P/sm8350-boot/build/Image" of=combined_boot.bin conv=notrunc status=none
dd if="$P/rog5-patched.dtb" of=combined_boot.bin bs=1024 seek=64 conv=notrunc status=none
dd if="$D/Image-sm8350" of=combined_boot.bin bs=1024 seek=1536 conv=notrunc status=none
dd if="$P/windows/uefi_payload.bin" of=combined_boot.bin bs=512k seek=83 conv=notrunc status=none
python3 "$P/mu_aloha_platforms/ImageResources/mkbootimg.py" \
    --kernel combined_boot.bin --ramdisk /dev/null \
    --dtb "$P/device-dump/live-device-tree.dtb" \
    --base 0x00000000 --kernel_offset 0x00080000 \
    --ramdisk_offset 0x01000000 --dtb_offset 0x01F00000 \
    --os_version 13.0.0 --os_patch_level 2023-01 \
    --header_version 2 --pagesize 4096 \
    --cmdline "clk_ignore_unused pd_ignore_unused" \
    -o osito-boot-rog5.img
shasum -a 256 osito-boot-rog5.img | cut -d' ' -f1
EOF
)" || die "remote build/pack failed"

mkdir -p "$OUT_DIR"
mac "cat ~/$MAC_DIR/pack/osito-boot-rog5.img" > "$OUT_REL.tmp"
LOCAL_SHA="$(sha256sum "$OUT_REL.tmp" | cut -d' ' -f1)"
[ "$LOCAL_SHA" = "$REMOTE_SHA" ] || die "checksum mismatch fetching boot.img ($LOCAL_SHA vs $REMOTE_SHA)"
mv -f "$OUT_REL.tmp" "$OUT_REL"
info "boot.img: $OUT_REL (sha256 ${LOCAL_SHA:0:16}...)"

# --- Optional: virt smoke test in local QEMU --------------------------------
# -m 4G because start.S paints the SM8350 framebuffer (0xE5000000) before it
# knows the platform; with less RAM nothing is mapped there and virt aborts.
if [ "$VIRT" = 1 ]; then
    QEMU="${QEMU:-$(command -v qemu-system-aarch64 || echo /c/msys64/mingw64/bin/qemu-system-aarch64.exe)}"
    [ -x "$QEMU" ] || die "qemu-system-aarch64 not found (set QEMU)"
    winpath() { if command -v cygpath >/dev/null; then cygpath -m "$1"; else printf '%s' "$1"; fi; }
    mac "cat ~/$MAC_DIR/Image-virt" > "$OUT_DIR/Image-virt"
    rm -f "$OUT_DIR/virt-serial.log"
    info "Booting the virt Image in QEMU (20 s)..."
    timeout 20 "$QEMU" -M virt,gic-version=3 -cpu cortex-a72 -m 4G \
        -display none -no-reboot -kernel "$(winpath "$OUT_DIR/Image-virt")" \
        -serial "file:$(winpath "$OUT_DIR/virt-serial.log")" || true
    if grep -q 'shell>' "$OUT_DIR/virt-serial.log"; then
        info "virt: reached shell> (log: arch/arm/build/rog5/virt-serial.log)"
    else
        tail -15 "$OUT_DIR/virt-serial.log" >&2
        die "virt: did not reach shell>"
    fi
fi

# --- Optional: flash --------------------------------------------------------
# `fastboot boot` bootloops on this device: the image has to be flashed. Only
# the active slot's boot partition is touched.
if [ "$FLASH" = 1 ]; then
    FB="${FASTBOOT:-$(command -v fastboot || echo /c/platform-tools/fastboot.exe)}"
    "$FB" devices | grep -q 'fastboot' || die "no device in fastboot mode (Power + Vol-Down)"
    SLOT="$("$FB" getvar current-slot 2>&1 | sed -n 's/^current-slot: *//p' | tr -d '\r')"
    [ -n "$SLOT" ] || die "could not read current-slot"
    info "Flashing boot_$SLOT and rebooting..."
    "$FB" flash "boot_$SLOT" "$OUT_REL"
    "$FB" reboot
fi

# --- Optional: load over USB --------------------------------------------------
# The raw Image, not boot.img: the running kernel jumps straight into it.
if [ "$USB" = 1 ]; then
    command -v powershell.exe >/dev/null || die "--usb needs powershell.exe (Windows)"
    mac "cat ~/$MAC_DIR/Image-sm8350" > "$OUT_DIR/Image-sm8350"
    info "Sending the kernel over USB..."
    powershell.exe -NoProfile -ExecutionPolicy Bypass \
        -File "$(cygpath -w "$SCRIPT_DIR/rog5-kload.ps1")" \
        -Image "$(cygpath -w "$OUT_DIR/Image-sm8350")"
fi
