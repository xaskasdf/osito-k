#!/bin/bash
#
# OsitoK x86-64 — QEMU runner for WSL2 with HARDWARE ACCELERATION (KVM).
#
# WHPX on Windows can't boot OVMF (it injects #GP on OVMF's early MTRR WRMSR in
# PlatformPei — see docs/windows-qemu-whpx.md). WSL2 exposes a real /dev/kvm via
# nested virtualization, and KVM emulates those MSRs correctly, so we get
# near-native speed and OVMF boots fine. This is the fast path for UT99/menu
# debugging where TCG (every instruction emulated) is the bottleneck.
#
# Run from a WSL2 (Debian) shell:
#   ./arch/x86/scripts/run-wsl-kvm.sh
#   OK_DISPLAY=gtk OK_NVME=/mnt/c/Users/xasko/osito-k/nvme_ut99.img \
#       ./arch/x86/scripts/run-wsl-kvm.sh
#
# The kernel.elf/boot.efi are built on the Windows side (msys2 clang); this
# script reads them from /mnt/c and stages everything onto the WSL ext4 fs
# (fast) before booting. Serial log -> $STAGE/serial.log.
set -e

info() { printf '\033[1;36m[run-wsl]\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[run-wsl] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

# Source build outputs live on the Windows side (msys2 build).
WIN_BUILD="${OK_WIN_BUILD:-/mnt/c/Users/xasko/osito-k/arch/x86/build}"
[ -f "$WIN_BUILD/kernel.elf" ] || die "no $WIN_BUILD/kernel.elf — build on Windows first (make -C arch/x86 CLANG=1)"
[ -f "$WIN_BUILD/boot.efi" ]   || die "no $WIN_BUILD/boot.efi"

command -v qemu-system-x86_64 >/dev/null || die "install qemu: sudo apt install -y qemu-system-x86 ovmf mtools"
command -v mformat >/dev/null            || die "install mtools: sudo apt install -y mtools"
[ -r /dev/kvm ] || die "/dev/kvm not readable — run: sudo usermod -aG kvm \$USER  (then restart WSL)"

# Stage onto native ext4 for speed (9p /mnt/c is slow for QEMU disk I/O).
STAGE="${OK_STAGE:-$HOME/osito-run}"
mkdir -p "$STAGE"
SERIAL="$STAGE/serial.log"; rm -f "$SERIAL"

info "Staging kernel.elf + boot.efi..."
cp -f "$WIN_BUILD/kernel.elf" "$STAGE/kernel.elf"
cp -f "$WIN_BUILD/boot.efi"   "$STAGE/boot.efi"

# ESP FAT image: boot.efi -> BOOTX64.EFI + kernel.elf + startup.nsh
info "Building ESP image..."
rm -f "$STAGE/esp.img"
dd if=/dev/zero of="$STAGE/esp.img" bs=1M count=64 status=none
mformat -i "$STAGE/esp.img" -F ::
mmd -i "$STAGE/esp.img" ::/EFI ::/EFI/BOOT
mcopy -i "$STAGE/esp.img" "$STAGE/boot.efi"  ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$STAGE/esp.img" "$STAGE/kernel.elf" ::/EFI/BOOT/kernel.elf
printf 'FS0:\\EFI\\BOOT\\BOOTX64.EFI\r\n' > "$STAGE/startup.nsh"
mcopy -i "$STAGE/esp.img" "$STAGE/startup.nsh" ::/startup.nsh

# OVMF firmware (pflash — KVM has no problem with the pflash ROMD path).
OVMF_CODE=""; OVMF_VARS_SRC=""
for c in /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd \
         /usr/share/qemu/OVMF_CODE.fd; do [ -f "$c" ] && OVMF_CODE="$c" && break; done
for v in /usr/share/OVMF/OVMF_VARS_4M.fd /usr/share/OVMF/OVMF_VARS.fd \
         /usr/share/qemu/OVMF_VARS.fd; do [ -f "$v" ] && OVMF_VARS_SRC="$v" && break; done
[ -n "$OVMF_CODE" ] || die "OVMF firmware not found — sudo apt install -y ovmf"
cp -f "$OVMF_VARS_SRC" "$STAGE/OVMF_VARS.fd"   # writable per-run vars copy

# Optional NVMe image. Persistent staging is the default; test helpers can set
# OK_NVME_FRESH=1 when guest writes must never leak into the next run.
NVME_ARGS=()
if [ -n "${OK_NVME:-}" ]; then
    [ -f "$OK_NVME" ] || die "OK_NVME not found: $OK_NVME"
    NVME_FRESH="${OK_NVME_FRESH:-0}"
    NVME_SNAPSHOT="${OK_NVME_SNAPSHOT:-0}"
    case "$NVME_FRESH" in
        0|1) ;;
        *) die "OK_NVME_FRESH must be 0 or 1" ;;
    esac
    case "$NVME_SNAPSHOT" in
        0|1) ;;
        *) die "OK_NVME_SNAPSHOT must be 0 or 1" ;;
    esac
    NVME_LOCAL="$STAGE/$(basename "$OK_NVME")"
    NVME_SOURCE_REAL="$(readlink -f "$OK_NVME")"
    NVME_LOCAL_REAL="$(readlink -m "$NVME_LOCAL")"
    if [ "$NVME_SOURCE_REAL" != "$NVME_LOCAL_REAL" ] &&
       { [ "$NVME_FRESH" = "1" ] || [ ! -f "$NVME_LOCAL" ] ||
         [ "$OK_NVME" -nt "$NVME_LOCAL" ]; }; then
        if [ "$NVME_FRESH" = "1" ]; then
            info "Staging fresh NVMe image onto ext4..."
        else
            info "Staging newer NVMe image onto ext4..."
        fi
        cp -f "$OK_NVME" "$NVME_LOCAL"
    fi
    NVME_SNAPSHOT_ARG=""
    if [ "$NVME_SNAPSHOT" = "1" ]; then
        NVME_SNAPSHOT_ARG=",snapshot=on"
        info "NVMe writes use a temporary QEMU snapshot"
    fi
    NVME_ARGS=(-drive "file=$NVME_LOCAL,format=raw,if=none,id=nvme0,cache=writeback,aio=threads$NVME_SNAPSHOT_ARG"
               -device "nvme,serial=deadbeef,drive=nvme0")
    info "NVMe: $NVME_LOCAL"
fi

DISPLAY_MODE="${OK_DISPLAY:-none}"
MEM="${OK_MEM:-2048M}"
CPU="${OK_CPU:-host}"   # host passthrough = max KVM speed

MON_ARGS=()
if [ "${OK_MONITOR:-0}" = "1" ]; then
    MON_HOST="${OK_MON_HOST:-127.0.0.1}"
    MON_PORT="${OK_MON_PORT:-55555}"
    case "$MON_PORT" in
        ''|*[!0-9]*) die "OK_MON_PORT must be an integer from 1 to 65535" ;;
    esac
    if [ "$MON_PORT" -lt 1 ] || [ "$MON_PORT" -gt 65535 ]; then
        die "OK_MON_PORT must be an integer from 1 to 65535"
    fi
    MON_ARGS=(-monitor "tcp:$MON_HOST:$MON_PORT,server,nowait")
    info "QEMU monitor -> tcp:$MON_HOST:$MON_PORT"
fi

AUDIO_ARGS=()
case "${OK_AUDIO:-none}" in
    none)
        ;;
    wav)
        AUDIO_WAV="${OK_AUDIO_WAV:-$STAGE/audio.wav}"
        AUDIO_RATE="${OK_AUDIO_RATE:-48000}"
        rm -f "$AUDIO_WAV"
        AUDIO_ARGS=(-audiodev "wav,id=audio0,path=$AUDIO_WAV,out.fixed-settings=on,out.frequency=$AUDIO_RATE,out.channels=2,out.format=s16"
                    -device intel-hda
                    -device "hda-output,audiodev=audio0")
        info "HDA output -> $AUDIO_WAV"
        ;;
    *)
        die "OK_AUDIO must be none or wav"
        ;;
esac

info "KVM accel, q35, smp 4, cpu $CPU, mem $MEM, display=$DISPLAY_MODE"
info "Serial log -> $SERIAL"

exec qemu-system-x86_64 \
    -accel kvm -cpu "$CPU" \
    -machine q35 -m "$MEM" -smp 4 \
    -drive if=pflash,format=raw,unit=0,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,unit=1,file="$STAGE/OVMF_VARS.fd" \
    -drive file="$STAGE/esp.img",format=raw,if=ide \
    "${NVME_ARGS[@]}" \
    -device virtio-net-pci,netdev=net0,disable-legacy=on,disable-modern=off \
    -netdev user,id=net0,hostfwd=udp::7778-:7777,hostfwd=tcp::50052-:50052 \
    -device qemu-xhci,id=usb \
    -device usb-kbd,bus=usb.0 \
    -device usb-tablet,bus=usb.0 \
    "${AUDIO_ARGS[@]}" \
    -display "$DISPLAY_MODE" \
    "${MON_ARGS[@]}" \
    -serial "file:$SERIAL" \
    -no-reboot
