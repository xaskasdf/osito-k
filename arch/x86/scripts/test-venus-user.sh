#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
X86_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$X86_DIR/build"
SERIAL_LOG="$BUILD_DIR/serial.log"
RUN_LOG="$BUILD_DIR/venus-user-qemu.log"
TEST_IMAGE="${QEMU_NVME_IMG:-$BUILD_DIR/venus-user-test.img}"
TEST_KEYS="${QEMU_TEST_KEYS:-e x e c spc v k t e s t ret}"
PASS_PATTERN="${QEMU_PASS_PATTERN:-VENUS-USER-ROUNDTRIP: OK}"
FAIL_PATTERN="${QEMU_FAIL_PATTERN:-VENUS-USER-ROUNDTRIP: .*failed}"
KEY_DELAY="${QEMU_KEY_DELAY:-0.35}"
KEY_RETRIES="${QEMU_KEY_RETRIES:-1}"
MONITOR_TIMEOUT="${QEMU_MONITOR_TIMEOUT:-10}"
BOOT_TIMEOUT="${QEMU_BOOT_TIMEOUT:-90}"
TEST_TIMEOUT="${QEMU_TEST_TIMEOUT:-60}"
QEMU_MEM="${QEMU_MEM:-4G}"
QEMU_SMP="${QEMU_SMP:-4}"
QEMU_GPU_HOSTMEM="${QEMU_GPU_HOSTMEM:-512M}"
QEMU_NO_KVM="${QEMU_NO_KVM:-1}"

[[ "$KEY_DELAY" =~ ^[0-9]+([.][0-9]+)?$ ]] || {
    echo "invalid QEMU_KEY_DELAY: $KEY_DELAY" >&2
    exit 2
}
[[ "$KEY_RETRIES" =~ ^[1-9][0-9]*$ ]] || {
    echo "invalid QEMU_KEY_RETRIES: $KEY_RETRIES" >&2
    exit 2
}
[[ "$BOOT_TIMEOUT" =~ ^[1-9][0-9]*$ ]] || {
    echo "invalid QEMU_BOOT_TIMEOUT: $BOOT_TIMEOUT" >&2
    exit 2
}
[[ "$TEST_TIMEOUT" =~ ^[1-9][0-9]*$ ]] || {
    echo "invalid QEMU_TEST_TIMEOUT: $TEST_TIMEOUT" >&2
    exit 2
}

[ -f "$TEST_IMAGE" ] || {
    echo "missing Venus test image: $TEST_IMAGE" >&2
    exit 1
}

rm -f "$RUN_LOG" "$SERIAL_LOG"
QEMU_ARGS=(--no-build --venus)
[ "$QEMU_NO_KVM" = "1" ] && QEMU_ARGS+=(--no-kvm)
QEMU_NVME_IMG="$TEST_IMAGE" QEMU_MEM="$QEMU_MEM" QEMU_SMP="$QEMU_SMP" \
QEMU_GPU_HOSTMEM="$QEMU_GPU_HOSTMEM" bash "$SCRIPT_DIR/qemu-test.sh" \
    "${QEMU_ARGS[@]}" >"$RUN_LOG" 2>&1 &
RUN_PID=$!

cleanup() {
    kill "$RUN_PID" 2>/dev/null || true
    bash "$SCRIPT_DIR/qemu-test.sh" --kill >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

booted=0
for _ in $(seq 1 "$BOOT_TIMEOUT"); do
    if grep -aq "osito> " "$SERIAL_LOG" 2>/dev/null; then
        booted=1
        break
    fi
    kill -0 "$RUN_PID" 2>/dev/null || break
    sleep 1
done
[ "$booted" -eq 1 ] || {
    cat "$RUN_LOG" >&2
    exit 1
}

{
    attempt=0
    while [ "$attempt" -lt "$KEY_RETRIES" ]; do
        for key in $TEST_KEYS; do
            printf 'sendkey %s 50\n' "$key"
            sleep "$KEY_DELAY"
        done
        attempt=$((attempt + 1))
    done
} | nc -U -w "$MONITOR_TIMEOUT" /tmp/qemu-monitor.sock >/dev/null

passed=0
for _ in $(seq 1 "$TEST_TIMEOUT"); do
    if grep -aqE "$PASS_PATTERN" "$SERIAL_LOG" 2>/dev/null; then
        passed=1
        break
    fi
    if grep -aqE "$FAIL_PATTERN|Unknown command|EXCEPTION|#PF|HALTED" \
        "$SERIAL_LOG" 2>/dev/null; then
        break
    fi
    kill -0 "$RUN_PID" 2>/dev/null || break
    sleep 1
done

grep -aE "\[VG3D\]|VENUS-USER-ROUNDTRIP|DXVK-D3D11-(DEVICE|READBACK|TEXTURE|RESOURCES|SHADERS|DRAW)|GRC-OSITOK-DXVK|GTAV-OSITOK-APP-SHELL|EXCEPTION|#PF|HALTED" \
    "$SERIAL_LOG" || true
[ "$passed" -eq 1 ]
