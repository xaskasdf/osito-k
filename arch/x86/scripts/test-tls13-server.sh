#!/bin/bash
# test-tls13-server.sh — boot the kernel with port-forward 19997→host,
# run openssl s_client -tls1_3 against it from the host, assert the
# handshake completes (which requires our server-side ClientHello
# parse, ServerHello build, x25519 ECDH, AES-GCM record framing,
# Certificate, ECDSA-P256 CertificateVerify, and Finished MAC all to
# be correct).

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
X86_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD="$X86_DIR/build"
[ -f "$BUILD/kernel.elf" ] || { echo "build first"; exit 1; }

OFS_TOOLS=/Users/pc/osito-a/tools/ositofs
NVME="$BUILD/nvme-tls13.img"
cp "$BUILD/nvme.img" "$NVME"

# Make sure cluster/tls-cert.der and cluster/tls-key.bin exist on the
# image.  Regenerate fresh each run so the test is hermetic.
bash "$SCRIPT_DIR/gen-cluster-cert.sh" "$NVME" >/dev/null
# Export the cert PEM for openssl s_client's -CAfile.
WORK=$(mktemp -d)
trap "rm -rf $WORK; kill $PID 2>/dev/null || true" EXIT
"$OFS_TOOLS/ositofs-read" "$NVME" "cluster/tls-cert.der" "$WORK/cert.der" >/dev/null
openssl x509 -inform DER -in "$WORK/cert.der" -outform PEM -out "$WORK/cert.pem"

ESP="$BUILD/esp-tls13.img"
dd if=/dev/zero of="$ESP" bs=1M count=64 status=none
mformat -i "$ESP" -F ::
mmd -i "$ESP" ::/EFI; mmd -i "$ESP" ::/EFI/BOOT
mcopy -i "$ESP" "$BUILD/boot.efi"  ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$ESP" "$BUILD/kernel.elf" ::/EFI/BOOT/kernel.elf
printf 'FS0:\\EFI\\BOOT\\BOOTX64.EFI\r\n' > /tmp/startup.nsh
mcopy -i "$ESP" /tmp/startup.nsh ::/startup.nsh

OVMF=/usr/local/Cellar/qemu/11.0.0/share/qemu/edk2-x86_64-code.fd
VARS=/tmp/ovmf_vars_tls13.fd
cp /usr/local/share/qemu/edk2-i386-vars.fd "$VARS" 2>/dev/null \
  || cp /usr/local/Cellar/qemu/11.0.0/share/qemu/edk2-i386-vars.fd "$VARS"
LOG="$BUILD/serial-tls13.log"; rm -f "$LOG"

# user-mode slirp with port-forward 19997 (host) → 19997 (guest)
qemu-system-x86_64 -accel hvf -cpu host \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF" \
  -drive if=pflash,format=raw,file="$VARS" \
  -drive file="$ESP",format=raw,if=ide \
  -drive file="$NVME",format=raw,if=none,id=nvme0,cache=none \
  -device nvme,serial=deadbeef,drive=nvme0 \
  -m 512M -machine q35 -smp 4 \
  -device virtio-net-pci,netdev=net0,disable-legacy=on,disable-modern=off \
  -netdev user,id=net0,hostfwd=tcp::19997-:19997 \
  -display none -serial file:"$LOG" -no-reboot >/dev/null 2>&1 &
PID=$!

# Wait for the TLS server listener to come up.
for _ in $(seq 1 60); do
    grep -aq "\[TLS1.3-S\] listener up on port 19997" "$LOG" 2>/dev/null && break
    sleep 1
done
if ! grep -aq "\[TLS1.3-S\] listener up" "$LOG"; then
    echo "  FAIL listener never came up"
    echo "=== tail serial ==="
    tail -30 "$LOG"
    exit 1
fi
echo "  OK   listener is up"

# Drive openssl s_client.  Allow self-signed via -CAfile (we generated it).
# Use -ign_eof so we send a tiny payload and the server can teardown.
echo "[test] driving openssl s_client -tls1_3 against 127.0.0.1:19997"
S_CLIENT_OUT=$(echo "GET / HTTP/1.0" | \
  openssl s_client -tls1_3 -connect 127.0.0.1:19997 \
    -CAfile "$WORK/cert.pem" -servername osito-a-node \
    -verify_return_error 2>&1 || true)

echo "=== s_client tail ==="
echo "$S_CLIENT_OUT" | tail -20

PASS=0; FAIL=0
if echo "$S_CLIENT_OUT" | grep -q "Verification:"; then PASS=$((PASS+1)); echo "  OK   s_client got cert"
  else FAIL=$((FAIL+1)); echo "  FAIL s_client never received cert"
fi
if grep -aq "\[TLS1.3-S\] ClientHello parsed" "$LOG"; then
    echo "  OK   server parsed ClientHello"; PASS=$((PASS+1))
else echo "  FAIL server didn't parse ClientHello"; FAIL=$((FAIL+1)); fi
if grep -aq "\[TLS1.3-S\] ServerHello sent" "$LOG"; then
    echo "  OK   server sent ServerHello"; PASS=$((PASS+1))
else echo "  FAIL server didn't send ServerHello"; FAIL=$((FAIL+1)); fi
if grep -aq "\[TLS1.3-S\] server flight {EE,Cert,CV,Fin} sent" "$LOG"; then
    echo "  OK   server sent encrypted flight"; PASS=$((PASS+1))
else echo "  FAIL server didn't send encrypted flight"; FAIL=$((FAIL+1)); fi
if grep -aq "\[TLS1.3-S\] handshake complete" "$LOG"; then
    echo "  OK   server reached handshake complete"; PASS=$((PASS+1))
else echo "  FAIL server didn't reach handshake complete"; FAIL=$((FAIL+1)); fi

echo
echo "=== summary: $PASS passed, $FAIL failed ==="
exit $([ $FAIL -eq 0 ] && echo 0 || echo 1)
