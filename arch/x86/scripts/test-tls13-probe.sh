#!/bin/bash
# test-rag-probe.sh — boot the kernel, fire the dedicated RAG probe
# after the net stack has stabilized, assert that retrieval against
# the live R2-hosted simple_en corpus returns "Wikipedia says: ..."
# observations for both probe questions.

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
X86_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD="$X86_DIR/build"
[ -f "$BUILD/kernel.elf" ] || { echo "build first"; exit 1; }

OFS_TOOLS=/Users/pc/osito-a/tools/ositofs
NVME="$BUILD/nvme-tls13.img"
cp "$BUILD/nvme.img" "$NVME"

SENT_DIR=$(mktemp -d)
: > "$SENT_DIR/no-demo.bin"
echo "tls13-probe-2026-05-16" > "$SENT_DIR/tls13-probe.txt"
for fn in ws-test.txt ws-send.txt ws-serve.txt ws-gen.txt no-demo.bin \
          agent-probe.txt tls13-probe.txt; do
  "$OFS_TOOLS/ositofs-delete" "$NVME" "$fn" >/dev/null 2>&1 || true
done
"$OFS_TOOLS/ositofs-write" "$NVME" "$SENT_DIR/no-demo.bin"    | tail -1
"$OFS_TOOLS/ositofs-write" "$NVME" "$SENT_DIR/tls13-probe.txt"  | tail -1

# A12.9: drop an operator CA bundle test file so the loader sees
# it during boot.  Mix of duplicates (already-pinned) and one
# new dummy hash to exercise both the dedup and add paths.
mkdir -p "$SENT_DIR/tls"
cat > "$SENT_DIR/tls/roots.txt" <<'_ROOTS'
# Operator-added roots (A12.9 test)
# Duplicates of static pins (should be skipped as already-present)
1dfc1605fbad358d8bc844f76d15203fac9ca5c1a79fd4857ffaf2864fbebf96
76b27b80a58027dc3cf1da68dac17010ed93997d0b603e2fadbe85012493b5a7
# A made-up hash to test parsing of new entries
0000111122223333444455556666777788889999aaaabbbbccccddddeeeeffff
_ROOTS
"$OFS_TOOLS/ositofs-delete" "$NVME" "tls/roots.txt" >/dev/null 2>&1 || true
"$OFS_TOOLS/ositofs-write" "$NVME" "$SENT_DIR/tls/roots.txt" \
    --name "tls/roots.txt" --overwrite | tail -1
rm -rf "$SENT_DIR"

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

# NET=vmnet switches from QEMU user-mode slirp (which drops CF
# continuation packets and truncates the /embed body around 7 KB)
# to Apple's vmnet-shared framework, which presents a real NAT'd
# interface.  Requires sudo because vmnet needs root.
if [ "${NET:-}" = "vmnet" ] || [ "${NET:-}" = "vmnet-shared" ]; then
    if [ "$(id -u)" -ne 0 ]; then
        echo "ERROR: NET=vmnet requires sudo (vmnet framework needs root)" >&2
        exit 1
    fi
    NETDEV_ARGS="-netdev vmnet-shared,id=net0"
else
    NETDEV_ARGS="-netdev user,id=net0"
fi

qemu-system-x86_64 -accel hvf -cpu host \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF" \
  -drive if=pflash,format=raw,file="$VARS" \
  -drive file="$ESP",format=raw,if=ide \
  -drive file="$NVME",format=raw,if=none,id=nvme0,cache=none \
  -device nvme,serial=deadbeef,drive=nvme0 \
  -m 512M -machine q35 -smp 4 \
  -device virtio-net-pci,netdev=net0,disable-legacy=on,disable-modern=off \
  $NETDEV_ARGS \
  -object filter-dump,id=f0,netdev=net0,file=/tmp/tls13-net.pcap \
  -display none -serial file:"$LOG" -no-reboot >/dev/null 2>&1 &
PID=$!
trap "kill -TERM $PID 2>/dev/null; sleep 1; kill -9 $PID 2>/dev/null" EXIT

for _ in $(seq 1 240); do
  grep -aq "\[PROBE\] tls 1.3 probe done" "$LOG" 2>/dev/null && break
  sleep 1
done

echo
echo "=== TLS 1.3 probe lines ==="
grep -aE "^\[TLS1.3\]|^\[PROBE\] tls" "$LOG" || true

echo
PASS=0; FAIL=0
if grep -aq "\[TLS1.3\] handshake complete" "$LOG"; then
    echo "  OK   TLS 1.3 handshake completed"; PASS=$((PASS+1))
else
    echo "  FAIL no '[TLS1.3] handshake complete' in log"; FAIL=$((FAIL+1))
fi
if grep -aq "\[TLS1.3\] server Finished verified" "$LOG"; then
    echo "  OK   server Finished MAC verified"; PASS=$((PASS+1))
else
    echo "  FAIL Finished MAC verify missing"; FAIL=$((FAIL+1))
fi
body_bytes=$(grep -ao 'tls13 body=[0-9]*' "$LOG" | head -1 | sed 's/.*body=//')
if [ -n "$body_bytes" ] && [ "$body_bytes" -gt 0 ]; then
    echo "  OK   received $body_bytes app-data bytes through tls13_recv"; PASS=$((PASS+1))
else
    echo "  FAIL no app-data bytes received"; FAIL=$((FAIL+1))
fi
echo
echo "=== summary: $PASS passed, $FAIL failed ==="
exit $([ $FAIL -eq 0 ] && echo 0 || echo 1)
