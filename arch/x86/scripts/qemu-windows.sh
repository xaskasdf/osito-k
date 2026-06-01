#!/bin/bash
#
# OsitoK x86-64 — QEMU runner para Windows (msys2 / mingw64)
#
# Hermano Windows de qemu-cocoa.sh. Diferencias clave vs macOS:
#   -accel hvf   ->  -accel whpx,kernel-irqchip=off   (Windows Hypervisor
#                    Platform; equivalente a HVF). Fallback a TCG si WHPX
#                    no esta disponible.
#   -display cocoa -> -display gtk  (ventana nativa via mingw qemu).
#   Firmware OVMF: el qemu de msys2 (mingw-w64-x86_64-qemu) trae edk2 en
#                  $MINGW_PREFIX/share/qemu/edk2-x86_64-code.fd (+ vars).
#
# Se invoca normalmente desde build-windows.ps1, pero corre solo bajo el
# shell mingw64 de msys2:
#   $ ./arch/x86/scripts/qemu-windows.sh
#
# Variables de entorno opcionales (las setea el .ps1):
#   OK_ACCEL=whpx|tcg        (default whpx, fallback tcg)
#   OK_DISPLAY=gtk|sdl|none  (default gtk)
#   OK_REBUILD=1             (fuerza make antes de correr)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
X86_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$X86_DIR/build"
EFI_BIN="$BUILD_DIR/boot.efi"
KERN_BIN="$BUILD_DIR/kernel.elf"
ESP_IMG="$BUILD_DIR/esp.img"

info()  { printf '\033[1;36m[qemu-win]\033[0m %s\n' "$*"; }
error() { printf '\033[1;31m[qemu-win] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

# --- Build si hace falta -----------------------------------------------------
if [ "${OK_REBUILD:-0}" = "1" ] || [ ! -f "$KERN_BIN" ] || [ ! -f "$EFI_BIN" ]; then
    info "Compilando boot.efi + kernel.elf (make -C arch/x86)..."
    make -C "$X86_DIR" || error "make fallo. Revisa el toolchain (x86_64-elf-gcc, gnu-efi)."
fi
[ -f "$KERN_BIN" ] || error "kernel.elf no encontrado en $KERN_BIN"
[ -f "$EFI_BIN" ]  || error "boot.efi no encontrado en $EFI_BIN"

# --- Firmware (edk2/OVMF del mingw qemu) -------------------------------------
MINGW_SHARE="${MINGW_PREFIX:-/mingw64}/share/qemu"
OVMF=""; OVMF_VARS=""; OVMF_PFLASH=0
for code in "$MINGW_SHARE/edk2-x86_64-code.fd" \
            "$MINGW_SHARE/edk2-x86_64-secure-code.fd" \
            /usr/share/qemu/OVMF.fd; do
    if [ -f "$code" ]; then
        OVMF="$code"
        case "$code" in
          *OVMF.fd) OVMF_PFLASH=0 ;;
          *) OVMF_PFLASH=1
             for v in "$MINGW_SHARE/edk2-i386-vars.fd" "$MINGW_SHARE/edk2-x86_64-vars.fd"; do
                 [ -f "$v" ] && OVMF_VARS="$v" && break
             done ;;
        esac
        break
    fi
done
[ -n "$OVMF" ] || error "Firmware edk2/OVMF no encontrado en $MINGW_SHARE. Instala: pacman -S mingw-w64-x86_64-qemu"

# --- ESP image (mtools, identico a qemu-cocoa.sh) ----------------------------
command -v mformat >/dev/null || error "mtools no instalado. pacman -S mtools"
info "Armando ESP image..."
dd if=/dev/zero of="$ESP_IMG" bs=1M count=64 status=none
mformat -i "$ESP_IMG" -F ::
mmd -i "$ESP_IMG" ::/EFI
mmd -i "$ESP_IMG" ::/EFI/BOOT
mcopy -i "$ESP_IMG" "$EFI_BIN"  ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$ESP_IMG" "$KERN_BIN" ::/EFI/BOOT/kernel.elf
printf 'FS0:\\EFI\\BOOT\\BOOTX64.EFI\r\n' > "$BUILD_DIR/startup.nsh"
mcopy -i "$ESP_IMG" "$BUILD_DIR/startup.nsh" ::/startup.nsh

if [ "$OVMF_PFLASH" = "1" ]; then
    VARS_TMP="$BUILD_DIR/ovmf_vars.fd"
    cp "$OVMF_VARS" "$VARS_TMP"
    BIOS_ARGS=(-drive "if=pflash,format=raw,readonly=on,file=$OVMF"
               -drive "if=pflash,format=raw,file=$VARS_TMP")
else
    BIOS_ARGS=(-bios "$OVMF")
fi

# --- NVMe (imagen de UT99/DOOM si existe) ------------------------------------
NVME_IMG="$BUILD_DIR/nvme.img"
NVME_ARGS=()
if [ -f "$NVME_IMG" ]; then
    info "NVMe image presente: $(du -h "$NVME_IMG" | cut -f1)"
    NVME_ARGS=(-drive "file=$NVME_IMG,format=raw,if=none,id=nvme0,cache=none"
               -device "nvme,serial=deadbeef,drive=nvme0")
fi

# --- Aceleracion: WHPX con fallback a TCG ------------------------------------
ACCEL="${OK_ACCEL:-whpx}"
DISPLAY_MODE="${OK_DISPLAY:-gtk}"
if [ "$ACCEL" = "whpx" ]; then
    ACCEL_ARGS=(-accel whpx,kernel-irqchip=off -cpu host)
    info "Accel: WHPX (Windows Hypervisor Platform). Si falla, reintenta con OK_ACCEL=tcg."
else
    ACCEL_ARGS=(-accel tcg -cpu max)
    info "Accel: TCG (emulacion software, mas lento)."
fi

SERIAL_LOG="$BUILD_DIR/serial.log"
rm -f "$SERIAL_LOG"
info "Serial log -> $SERIAL_LOG"
info "Lanzando QEMU (display=$DISPLAY_MODE)..."

set +e
qemu-system-x86_64 "${ACCEL_ARGS[@]}" \
    "${BIOS_ARGS[@]}" \
    -drive file="$ESP_IMG",format=raw,if=ide \
    "${NVME_ARGS[@]}" \
    -m 512M \
    -machine q35 \
    -smp 4 \
    -device virtio-net-pci,netdev=net0,disable-legacy=on,disable-modern=off \
    -netdev user,id=net0,hostfwd=udp::7778-:7777,hostfwd=tcp::50052-:50052 \
    -device qemu-xhci,id=usb \
    -device usb-kbd,bus=usb.0 \
    -device usb-tablet,bus=usb.0 \
    -display "$DISPLAY_MODE" \
    -serial file:"$SERIAL_LOG" \
    -no-reboot
RC=$?
set -e

if [ $RC -ne 0 ] && [ "$ACCEL" = "whpx" ]; then
    info "QEMU salio con codigo $RC. Si fue por WHPX, reintenta:  OK_ACCEL=tcg $0"
fi
exit $RC
