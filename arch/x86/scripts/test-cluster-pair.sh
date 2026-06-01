#!/bin/bash
# test-cluster-pair.sh — boot TWO osito-k kernels on the same
# vmnet-shared, assert they auto-discover each other via UDP
# multicast, exchange RPC heartbeats, transition through STALE/DEAD
# when one is killed, and re-join cleanly on restart.
#
# Run:   sudo NET=vmnet bash arch/x86/scripts/test-cluster-pair.sh
#        (multicast across two VMs requires the vmnet bridge —
#         user-mode slirp does NOT propagate between VMs)
#
# Ported from osito-a; paths adapted to /Users/pc/osito-k.

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
X86_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_DIR="$(cd "$X86_DIR/../.." && pwd)"
BUILD="$X86_DIR/build"
[ -f "$BUILD/kernel.elf" ] || { echo "build first"; exit 1; }
if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: cluster test needs sudo (vmnet bridge is root-only)" >&2
    exit 1
fi

OFS_TOOLS="$REPO_DIR/tools/ositofs"

# Shared 32-byte cluster PSK (oict-key.txt). Both VMs MUST carry the
# IDENTICAL key or HMAC-SHA256 heartbeat verify fails closed and the
# peers never transition to ALIVE. Generated once here, written into
# both images by prepare_nvme.
CLUSTER_KEY=$(mktemp)
head -c 32 /dev/urandom > "$CLUSTER_KEY"

prepare_nvme() {
    local name=$1
    local img="$BUILD/nvme-cluster-$name.img"
    cp "$(readlink "$BUILD/nvme.img" || echo "$BUILD/nvme.img")" "$img"
    # Provision the shared cluster PSK so HEARTBEAT verify succeeds.
    "$OFS_TOOLS/ositofs-delete" "$img" oict-key.txt >/dev/null 2>&1 || true
    "$OFS_TOOLS/ositofs-write"  "$img" "$CLUSTER_KEY" --name oict-key.txt --overwrite >/dev/null
    # Disable the boot-time UT99/DOOM auto-launch (shell.c) — it would
    # monopolize the BSP with the win32 exception cascade and starve the
    # cluster-tick kthread. The base image is "UT99-Full"; we only need
    # brandon-tiny.gguf for the agent, not the game.
    "$OFS_TOOLS/ositofs-delete" "$img" UnrealTournament.exe >/dev/null 2>&1 || true
    "$OFS_TOOLS/ositofs-delete" "$img" DOOM.EXE             >/dev/null 2>&1 || true
    # Drop no-demo.bin on BOTH VMs — the agent auto-demo task ties up
    # slot 0 for 30-60 s of brandon-tiny generation, which (a) starves
    # the IC-broadcaster kthread on the same BSP so the OTHER VM
    # marks us STALE, and (b) queues our delegation task behind a slow
    # full-inference run.  Removing the demo lets the cluster heartbeat
    # cadence stay regular and slot 0 stay available for delegation.
    local empty
    empty=$(mktemp)
    : > "$empty"
    "$OFS_TOOLS/ositofs-delete" "$img" no-demo.bin >/dev/null 2>&1 || true
    "$OFS_TOOLS/ositofs-write"  "$img" "$empty" --name no-demo.bin --overwrite >/dev/null
    rm -f "$empty"
    # VM A initiates delegation — drop the sentinel that gates the probe.
    if [ "$name" = a ]; then
        local sent
        sent=$(mktemp)
        echo "delegate-2026-06-01" > "$sent"
        "$OFS_TOOLS/ositofs-delete" "$img" cluster-delegate.txt >/dev/null 2>&1 || true
        "$OFS_TOOLS/ositofs-write"  "$img" "$sent" --name cluster-delegate.txt --overwrite >/dev/null
        rm -f "$sent"
    fi
    echo "$img"
}

prepare_esp() {
    local name=$1
    local esp="$BUILD/esp-cluster-$name.img"
    dd if=/dev/zero of="$esp" bs=1M count=64 status=none
    mformat -i "$esp" -F ::
    mmd -i "$esp" ::/EFI; mmd -i "$esp" ::/EFI/BOOT
    mcopy -i "$esp" "$BUILD/boot.efi"  ::/EFI/BOOT/BOOTX64.EFI
    mcopy -i "$esp" "$BUILD/kernel.elf" ::/EFI/BOOT/kernel.elf
    printf 'FS0:\\EFI\\BOOT\\BOOTX64.EFI\r\n' > /tmp/startup.nsh
    mcopy -i "$esp" /tmp/startup.nsh ::/startup.nsh
    echo "$esp"
}

OVMF=/usr/local/Cellar/qemu/11.0.0/share/qemu/edk2-x86_64-code.fd

boot_vm() {
    # boot_vm <name> <mac>
    local name=$1 mac=$2
    local nvme esp vars log
    nvme=$(prepare_nvme "$name")
    esp=$(prepare_esp  "$name")
    vars="/tmp/ovmf_vars_cluster-$name.fd"
    cp /usr/local/share/qemu/edk2-i386-vars.fd "$vars" 2>/dev/null \
      || cp /usr/local/Cellar/qemu/11.0.0/share/qemu/edk2-i386-vars.fd "$vars"
    log="$BUILD/serial-cluster-$name.log"; rm -f "$log"
    qemu-system-x86_64 -accel hvf -cpu host \
      -drive if=pflash,format=raw,readonly=on,file="$OVMF" \
      -drive if=pflash,format=raw,file="$vars" \
      -drive file="$esp",format=raw,if=ide \
      -drive file="$nvme",format=raw,if=none,id=nvme0,cache=none \
      -device nvme,serial=deadbeef,drive=nvme0 \
      -m 512M -machine q35 -smp 2 \
      -device virtio-net-pci,netdev=net0,disable-legacy=on,disable-modern=off,mac="$mac" \
      -netdev vmnet-shared,id=net0 \
      -display none -serial file:"$log" -no-reboot >/dev/null 2>&1 &
    echo $!
}

PID_A=$(boot_vm a 52:54:00:12:34:01)
PID_B=$(boot_vm b 52:54:00:12:34:02)
LOG_A="$BUILD/serial-cluster-a.log"
LOG_B="$BUILD/serial-cluster-b.log"

cleanup() {
    kill -TERM $PID_A $PID_B 2>/dev/null || true
    sleep 1
    kill -9   $PID_A $PID_B 2>/dev/null || true
}
trap cleanup EXIT

wait_for_line() {
    # wait_for_line <log> <pattern> <timeout_s>
    local log=$1 pat=$2 to=$3
    for _ in $(seq 1 "$to"); do
        grep -aq "$pat" "$log" 2>/dev/null && return 0
        sleep 1
    done
    return 1
}

# Generous discovery window: under dual-VM HVF contention each kernel
# needs to boot + mount OsitoFS + reload the PSK before its heartbeats
# verify, and BOTH must be up before either goes ALIVE.
echo "[test] waiting for both nodes to discover each other (up to 180s)..."
wait_for_line "$LOG_A" "\[CLUSTER\] peer .* ALIVE" 180 || true
wait_for_line "$LOG_B" "\[CLUSTER\] peer .* ALIVE" 180 || true

# From here on, a failing assertion must NOT abort the run — we want the
# full PASS/FAIL summary. (osito-a's set -e survived only because every
# assertion passed; make the harness robust to partial failure.)
set +e

PASS=0; FAIL=0
report() { if [ "$1" = 0 ]; then echo "  OK   $2"; PASS=$((PASS+1));
           else                  echo "  FAIL $2"; FAIL=$((FAIL+1)); fi; }

# T1: both saw each other ALIVE
grep -aq "\[CLUSTER\] peer .* ALIVE" "$LOG_A"; report $? "A saw B → ALIVE"
grep -aq "\[CLUSTER\] peer .* ALIVE" "$LOG_B"; report $? "B saw A → ALIVE"

# T2: cluster init line on both
grep -aq "\[CLUSTER\] init: max_peers=16" "$LOG_A"; report $? "A cluster_init ran"
grep -aq "\[CLUSTER\] init: max_peers=16" "$LOG_B"; report $? "B cluster_init ran"

# T2.5: delegation BEFORE kill cycle.
echo "[test] waiting for VM A to complete delegation (up to 300s)..."
wait_for_line "$LOG_A" "\[CLUSTER-PROBE\] delegation" 300 || true
rc=0; grep -aq "\[CLUSTER-PROBE\] delegating to" "$LOG_A" || rc=1
report $rc "A initiated delegation"
rc=0; grep -aq "\[CLUSTER-PROBE\] delegation OK" "$LOG_A" || rc=1
report $rc "A received delegation reply"

# T3: kill B and wait for STALE then DEAD on A
echo "[test] killing B; waiting for STALE → DEAD on A (up to 45s)..."
cp "$LOG_B" "$LOG_B.first" 2>/dev/null || true
kill -TERM $PID_B 2>/dev/null || true
sleep 1; kill -9 $PID_B 2>/dev/null || true
wait_for_line "$LOG_A" "\[CLUSTER\] peer .* → STALE" 25 || true
rc=0; grep -aq "\[CLUSTER\] peer .* → STALE" "$LOG_A" || rc=1
report $rc "A marked B STALE within 25s"
wait_for_line "$LOG_A" "\[CLUSTER\] peer .* → DEAD" 25 || true
rc=0; grep -aq "\[CLUSTER\] peer .* → DEAD" "$LOG_A" || rc=1
report $rc "A marked B DEAD within 25s"

# T4: epoch advanced past 0
if grep -aq "epoch=[1-9]" "$LOG_A"; then
    report 0 "A epoch advanced past 0"
else
    report 1 "A epoch advanced past 0"
fi

# T5: re-boot B and confirm A re-admits it
echo "[test] re-booting B; waiting for A to re-admit (up to 40s)..."
PID_B=$(boot_vm b 52:54:00:12:34:02)
N_ALIVE_BEFORE=$(grep -ac "ALIVE" "$LOG_A" || echo 0)
for _ in $(seq 1 40); do
    N_NOW=$(grep -ac "ALIVE" "$LOG_A" || echo 0)
    if [ "$N_NOW" -gt "$N_ALIVE_BEFORE" ]; then break; fi
    sleep 1
done
N_NOW=$(grep -ac "ALIVE" "$LOG_A" || echo 0)
if [ "$N_NOW" -gt "$N_ALIVE_BEFORE" ]; then
    report 0 "A re-admitted B after restart"
else
    report 1 "A re-admitted B after restart"
fi

echo
echo "=== summary: $PASS passed, $FAIL failed ==="
exit $([ $FAIL -eq 0 ] && echo 0 || echo 1)
