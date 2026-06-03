#!/bin/bash
#
# OsitoK x86-64 — QEMU runner para Windows (MSYS2 / MINGW64), TCG multi-thread.
#
# WHPX (aceleración por hardware) NO sirve con OsitoK en Windows: WHPX rechaza
# el WRMSR temprano que OVMF hace en PlatformPei -> #GP, antes de llegar a
# nuestro código. Es una limitación upstream de QEMU sin workaround por línea
# de comandos (probado: pflash/-bios, smp 1/4, smm=off, disable_s3, -x2apic...).
# Ver docs/windows-qemu-whpx.md. Por eso corremos TCG multi-thread (los 4 vCPU
# en 4 threads del host), que bootea OsitoK completo (self-tests PASS).
#
# Uso (desde un shell MSYS2 MINGW64):
#   ./arch/x86/scripts/run-windows.sh                       # headless, serial -> build/serial.log
#   OK_DISPLAY=gtk ./arch/x86/scripts/run-windows.sh        # ventana interactiva
#   OK_BUILD=1 ./arch/x86/scripts/run-windows.sh            # recompila kernel.elf + boot.efi (CLANG=1) antes
#   OK_NVME=/c/Users/xasko/osito-k/nvme_ut99.img OK_DISPLAY=gtk ./arch/x86/scripts/run-windows.sh
#                                                           # ^ el run de UT99: adjunta el NVMe y abre la ventana
#
# El [NAMEHASH] del walker Phase 7l aparece en build/serial.log durante el
# package-load del engine.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
X86_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD="$X86_DIR/build"
SHARE="${MINGW_PREFIX:-/mingw64}/share/qemu"

info() { printf '\033[1;36m[run-win]\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[run-win] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

command -v qemu-system-x86_64 >/dev/null || die "falta qemu (pacman -S mingw-w64-x86_64-qemu)"
command -v mformat >/dev/null            || die "falta mtools (pacman -S mingw-w64-x86_64-mtools)"

if [ "${OK_BUILD:-0}" = "1" ]; then
    info "Compilando kernel.elf + boot.efi (clang)..."
    make -C "$X86_DIR" CLANG=1 build/boot.efi build/kernel.elf
fi
[ -f "$BUILD/kernel.elf" ] || die "no hay build/kernel.elf — corré con OK_BUILD=1"
[ -f "$BUILD/boot.efi" ]   || die "no hay build/boot.efi — corré con OK_BUILD=1"

cd "$BUILD"

# ESP FAT image: boot.efi -> BOOTX64.EFI + kernel.elf + startup.nsh
info "Armando ESP image..."
rm -f esp.img
dd if=/dev/zero of=esp.img bs=1M count=64 status=none
mformat -i esp.img -F ::
mmd -i esp.img ::/EFI ::/EFI/BOOT
mcopy -i esp.img boot.efi  ::/EFI/BOOT/BOOTX64.EFI
mcopy -i esp.img kernel.elf ::/EFI/BOOT/kernel.elf
printf 'FS0:\\EFI\\BOOT\\BOOTX64.EFI\r\n' > startup.nsh
mcopy -i esp.img startup.nsh ::/startup.nsh

# Unified OVMF (vars+code) usada con -bios. WHPX no puede emular el dispositivo
# pflash (ROMD); -bios lo evita. TCG funciona con -bios igual, y deja la imagen
# lista por si algún día parcheamos WHPX.
[ -f ovmf-unified.fd ] || cat "$SHARE/edk2-i386-vars.fd" "$SHARE/edk2-x86_64-code.fd" > ovmf-unified.fd

# NVMe opcional (p.ej. la imagen de UT99)
NVME_ARGS=()
if [ -n "${OK_NVME:-}" ]; then
    [ -f "$OK_NVME" ] || die "OK_NVME no existe: $OK_NVME"
    # cache=writeback + aio=threads: QEMU on Windows can't do O_DIRECT
    # (cache=none) writes — they fail with "aio failed: Invalid argument".
    NVME_ARGS=(-drive "file=$OK_NVME,format=raw,if=none,id=nvme0,cache=writeback,aio=threads"
               -device "nvme,serial=deadbeef,drive=nvme0")
    info "NVMe: $OK_NVME ($(du -h "$OK_NVME" | cut -f1))"
fi

DISPLAY_MODE="${OK_DISPLAY:-none}"
# 2 GB by default so the gcc sysroot install fits (283 MB compressed +
# 748 MB inflate buffer ≈ 1 GB peak in the kernel heap, which auto-grows
# with RAM). Override with OK_MEM (e.g. OK_MEM=512M for the as/ld-only run).
MEM="${OK_MEM:-2048M}"

# Optional packet capture (OK_PCAP=1) → build/net.pcap, parse with pcap.py.
PCAP_ARGS=()
if [ "${OK_PCAP:-0}" = "1" ]; then
    rm -f net.pcap
    PCAP_ARGS=(-object filter-dump,id=dump0,netdev=net0,file=net.pcap)
    info "Packet capture -> $BUILD/net.pcap"
fi

SERIAL="$BUILD/serial.log"; rm -f "$SERIAL"
# TCG emulates every guest instruction in software; `-cpu max` makes the
# guest take AVX/AES-NI/PCLMULQDQ paths that TCG then emulates, which can be
# slower than scalar code under TCG. OK_CPU lets us A/B different models
# (e.g. Haswell, Nehalem, qemu64) to find the fastest under emulation.
CPU="${OK_CPU:-max}"
info "TCG multi-thread, q35, smp 4, cpu $CPU, mem $MEM, display=$DISPLAY_MODE"
info "Serial log -> $SERIAL"

qemu-system-x86_64 \
    -accel tcg,thread=multi -cpu "$CPU" \
    -machine q35 -m "$MEM" -smp 4 \
    -bios ovmf-unified.fd \
    -drive file=esp.img,format=raw,if=ide \
    "${NVME_ARGS[@]}" \
    -device virtio-net-pci,netdev=net0,disable-legacy=on,disable-modern=off \
    -netdev user,id=net0,hostfwd=udp::7778-:7777,hostfwd=tcp::50052-:50052 \
    "${PCAP_ARGS[@]}" \
    -device qemu-xhci,id=usb \
    -device usb-kbd,bus=usb.0 \
    -device usb-tablet,bus=usb.0 \
    -display "$DISPLAY_MODE" \
    -serial file:serial.log \
    -no-reboot
