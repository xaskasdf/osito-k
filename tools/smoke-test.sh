#!/bin/bash
#
# OsitoK — smoke test del kernel x86-64 con red activa.
#
# 1. Crea ESP mínima con boot.efi + kernel.elf.
# 2. Lanza QEMU headless con NIC e1000 SLIRP + port forwards:
#       host 5777/udp → guest 7777/udp  (UDP prompt server)
#       host 8080/tcp → guest 8080/tcp  (httpd HTTP server)
# 3. Espera que la línea "[DHCP] Configured: IP=" aparezca en
#    el serial log (DHCP exitoso) o el kernel llegue al shell.
# 4. Verifica red — opcional, requiere herramientas en host.
#
# Uso:
#   tools/smoke-test.sh           # 30s default
#   tools/smoke-test.sh 60        # 60s
#   ACCEL=tcg tools/smoke-test.sh # forzar emulación (sin HVF/KVM)
#

set -euo pipefail

TIMEOUT="${1:-30}"
X86="$(cd "$(dirname "$0")/../arch/x86" && pwd)"
BUILD="$X86/build"
ESP="$BUILD/esp.img"
COM1_LOG="$BUILD/com1.log"

[ -f "$BUILD/boot.efi"   ] || { echo "boot.efi missing — run: make -C arch/x86"; exit 1; }
[ -f "$BUILD/kernel.elf" ] || { echo "kernel.elf missing — run: make -C arch/x86"; exit 1; }

# OVMF detection
OVMF=""; OVMF_PFLASH=0; OVMF_VARS=""
for f in /usr/share/qemu/OVMF.fd /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF_CODE.fd; do
    [ -f "$f" ] && OVMF="$f" && break
done
if [ -z "$OVMF" ]; then
    for d in /usr/local/Cellar/qemu/*/share/qemu /opt/homebrew/Cellar/qemu/*/share/qemu \
             /opt/homebrew/share/qemu /usr/local/share/qemu; do
        if [ -f "$d/edk2-x86_64-code.fd" ]; then
            OVMF="$d/edk2-x86_64-code.fd"
            for v in "$d/edk2-i386-vars.fd" "$d/edk2-x86_64-vars.fd"; do
                [ -f "$v" ] && OVMF_VARS="$v" && break
            done
            OVMF_PFLASH=1
            break
        fi
    done
fi
[ -n "$OVMF" ] || { echo "OVMF firmware not found"; exit 1; }
echo "[+] OVMF: $OVMF"

# Build ESP con kernel.elf
echo "[+] Building ESP image..."
dd if=/dev/zero of="$ESP" bs=1M count=64 status=none
mformat -i "$ESP" -F ::
mmd -i "$ESP" ::/EFI
mmd -i "$ESP" ::/EFI/BOOT
mcopy -i "$ESP" "$BUILD/boot.efi"   ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$ESP" "$BUILD/kernel.elf" ::/EFI/BOOT/kernel.elf
printf 'FS0:\\EFI\\BOOT\\BOOTX64.EFI\r\n' > /tmp/startup.nsh
mcopy -i "$ESP" /tmp/startup.nsh ::/startup.nsh

if [ "$OVMF_PFLASH" = "1" ]; then
    VARS_TMP="$BUILD/ovmf_vars.fd"
    cp "$OVMF_VARS" "$VARS_TMP"
    BIOS_ARGS="-drive if=pflash,format=raw,readonly=on,file=$OVMF \
               -drive if=pflash,format=raw,file=$VARS_TMP"
else
    BIOS_ARGS="-bios $OVMF"
fi

rm -f "$COM1_LOG"

# Acelerador: HVF / KVM / TCG
ACCEL="${ACCEL:-}"
if [ -z "$ACCEL" ]; then
    if [ "$(uname)" = "Darwin" ] && qemu-system-x86_64 -accel help 2>&1 | grep -q hvf; then
        ACCEL="hvf"
    elif [ -e /dev/kvm ] && qemu-system-x86_64 -accel help 2>&1 | grep -q kvm; then
        ACCEL="kvm"
    else
        ACCEL="tcg"
    fi
fi
echo "[+] accel: $ACCEL"

if [ "$ACCEL" = "hvf" ] || [ "$ACCEL" = "kvm" ]; then
    CPU_ARGS="-cpu host"
else
    CPU_ARGS="-cpu qemu64"
fi

# NIC SLIRP con DHCP server interno + port forwards.  El driver i211 del
# kernel maneja device_id 8086:1539; QEMU emula igb (8086:10C9, 82576) o
# e1000 — ninguno matchea 1539 directo, pero el kernel cae al fallback
# estático (10.0.2.15) si DHCP falla, así que de todas formas hay red.
NET_ARGS="-netdev user,id=net0,hostfwd=udp::5777-:7777,hostfwd=tcp::8080-:8080
          -device igb,netdev=net0,mac=52:54:00:12:34:56
          -object filter-dump,id=f1,netdev=net0,file=$BUILD/net.pcap"

echo "[+] Booting QEMU headless (timeout ${TIMEOUT}s)..."
echo "    COM1 → $COM1_LOG"
echo "    Net  → host udp/5777 → guest udp/7777 (prompt)"
echo "         → host tcp/8080 → guest tcp/8080 (httpd)"

( qemu-system-x86_64 \
    -accel "$ACCEL" \
    $BIOS_ARGS \
    -drive file="$ESP",format=raw,if=ide \
    $NET_ARGS \
    -m 512M -machine q35 $CPU_ARGS -smp 1 \
    -display none \
    -serial file:"$COM1_LOG" \
    -no-reboot -no-shutdown \
    >/dev/null 2>&1 ) &

QEMU_PID=$!
trap 'kill "$QEMU_PID" 2>/dev/null || true' EXIT

# Espera con poll.  Termina cuando vemos el prompt del shell o cuando
# detectamos DHCP configurado, lo que ocurra primero.
SECS=0
DHCP_OK=0
SHELL_UP=0
while [ "$SECS" -lt "$TIMEOUT" ]; do
    sleep 1
    SECS=$((SECS+1))
    if [ -f "$COM1_LOG" ]; then
        if [ "$DHCP_OK" = "0" ] && grep -q "DHCP] Configured" "$COM1_LOG" 2>/dev/null; then
            DHCP_OK=1
            echo "[+] DHCP configurado tras ${SECS}s"
        fi
        if grep -qE "osito>|Type 'help' for commands" "$COM1_LOG" 2>/dev/null; then
            SHELL_UP=1
            echo "[+] Shell up tras ${SECS}s — cerrando"
            break
        fi
    fi
done

# Mantener QEMU 2 segundos más para que se estabilice si DHCP falló y
# vemos el fallback estático.
sleep 2

kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true

echo
echo "═══ COM1 (kernel) ═══"
[ -f "$COM1_LOG" ] && tail -60 "$COM1_LOG" || echo "(vacío)"

echo
echo "═══ Verificación ═══"
PASS=0; FAIL=0
check() {
    if grep -qE "$2" "$1" 2>/dev/null; then
        echo "  [OK]   '$2'"; PASS=$((PASS+1))
    else
        echo "  [MISS] '$2'"; FAIL=$((FAIL+1))
    fi
}
check "$COM1_LOG" "NIC detected"
check "$COM1_LOG" "(\\[DHCP\\] Configured|\\[NET\\] IP)"
check "$COM1_LOG" "(osito>|Type 'help')"

echo
echo "  $PASS pass / $FAIL miss"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
