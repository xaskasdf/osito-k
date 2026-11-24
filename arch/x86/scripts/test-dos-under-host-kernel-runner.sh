#!/bin/bash
# Harness-only checks; fake QEMU results are never DOS conformance evidence.
set -euo pipefail
if [ "${OSITO_NESTED_RUNNER_FAKE:-0}" = 1 ]; then
    if [ "${1:-}" = --version ]; then echo 'fake outer QEMU'; exit 0; fi
    printf 'called\n' >> "$OSITO_NESTED_FAKE_CALLS"
    if [ -n "${OSITO_NESTED_MUTATE:-}" ]; then
        printf 'intentionally invalid later source {\n' > "$OSITO_NESTED_MUTATE"
    fi
    run=
    while [ "$#" -gt 0 ]; do
        if [ "$1" = -pidfile ]; then run=${2%/outer.pid}; shift; fi
        shift
    done
    [ -n "$run" ]
    case ${OSITO_NESTED_FAKE_RESULT:-pass} in
        pass) printf '0\n' > "$run/output/inner.exit" ;;
        fail) printf '7\n' > "$run/output/inner.exit" ;;
        duplicate) printf '0\n0\n' > "$run/output/inner.exit" ;;
        missing) : ;;
        malformed) printf '00\n' > "$run/output/inner.exit" ;;
        range) printf '256\n' > "$run/output/inner.exit" ;;
        qemu-fail) exit 42 ;;
        timeout) exit 124 ;;
        *) exit 2 ;;
    esac
    exit 0
fi
script_dir=$(cd "$(dirname "$0")" && pwd)
runner=$script_dir/run-dos-under-host-kernel.sh
scratch=$(mktemp -d /tmp/ositonestedtest.XXXXXXXX)
cleanup() {
    [ "$(realpath -e "$scratch")" = "$scratch" ] && [[ "$scratch" == /tmp/ositonestedtest.* ]] || return 1
    rm -rf --one-file-system -- "$scratch"
}
trap cleanup EXIT
cp "$0" "$scratch/fake-qemu"
chmod +x "$scratch/fake-qemu"
export OSITO_OUTER_QEMU="$scratch/fake-qemu" OSITO_NESTED_RUNNER_FAKE=1
export OSITO_NESTED_FAKE_CALLS="$scratch/calls"
export OSITO_TEST_DISK_ROOT="$scratch/disks"
unset OSITO_TEST_DISK_DIR QEMU_ESP_IMG
base="$scratch/payload with spaces"
mkdir -p "$base/sources"
printf 'kernel\n' > "$scratch/kernel"
printf 'frozen payload\n' > "$base/initramfs.cpio.gz"
printf 'frozen Osito-K\n' > "$base/kernel.elf"
printf 'frozen source\n' > "$base/sources/init.c"
for metadata in root.sha256 dependencies.txt inner-qemu-version.txt; do
    printf 'frozen %s\n' "$metadata" > "$base/$metadata"
done
(cd "$base"; sha256sum initramfs.cpio.gz kernel.elf) > "$base/payload.sha256"
(cd "$base/sources"; sha256sum init.c) > "$base/sources.sha256"
rc=0
bash "$runner" > "$scratch/usage.log" 2>&1 || rc=$?
[ "$rc" = 2 ]
OSITO_KVM_TEST_KERNEL_ARGS='kvm_amd.nrips=0' bash "$runner" "$scratch/kernel" \
    "$scratch/reused" "$base" > "$scratch/reused.log" 2>&1
for artifact in initramfs.cpio.gz kernel.elf root.sha256 dependencies.txt inner-qemu-version.txt; do
    cmp "$base/$artifact" "$scratch/reused/$artifact"
done
cmp "$base/sources/init.c" "$scratch/reused/sources/init.c"
cmp "$runner" "$scratch/reused/runner.sh"
grep -q 'kvm_amd.nrips=0' "$scratch/reused/outer-command.txt"
[ ! -e "$(cat "$scratch/reused/transport.txt")" ]
[ -z "$(find "$scratch/disks/runs" -mindepth 1 -print -quit)" ]
echo 'PASS frozen payload/source/version reuse, spaces, kernel arguments, transport and lease cleanup'
(cd "$script_dir/.."; bash scripts/run-dos-under-host-kernel.sh "$scratch/kernel" \
    "$scratch/relative" "$base") > "$scratch/relative.log" 2>&1
echo 'PASS relative invocation outside repository root'
clone="$scratch/clone/arch/x86/scripts"
mkdir -p "$clone"
cp "$runner" "$script_dir/qemu-test-disks.sh" "$clone/"
OSITO_NESTED_MUTATE="$clone/run-dos-under-host-kernel.sh" bash \
    "$clone/run-dos-under-host-kernel.sh" "$scratch/kernel" "$scratch/live-edit" "$base" \
    > "$scratch/live-edit.log" 2>&1
cmp "$runner" "$scratch/live-edit/runner.sh"
[ "$(cat "$scratch/live-edit/outer.exit")" = 0 ]
echo 'PASS an edit to the launch script cannot change an already-running invocation'
for result in fail duplicate missing malformed range qemu-fail timeout; do
    rc=0
    OSITO_NESTED_FAKE_RESULT=$result bash "$runner" "$scratch/kernel" \
        "$scratch/$result" "$base" > "$scratch/$result.log" 2>&1 || rc=$?
    case $result in
        fail) [ "$rc" = 7 ] ;;
        duplicate|missing|malformed|range) [ "$rc" = 125 ] ;;
        qemu-fail) [ "$rc" = 42 ] ;;
        timeout) [ "$rc" = 124 ] ;;
    esac
    [ ! -e "$(cat "$scratch/$result/transport.txt")" ]
done
echo 'PASS inner failures, invalid/missing/duplicate status, outer failures and timeout status'
before=$(sha256sum "$scratch/reused/payload.sha256")
calls=$(wc -l < "$OSITO_NESTED_FAKE_CALLS")
rc=0
bash "$runner" "$scratch/kernel" "$scratch/reused" "$base" > "$scratch/existing.log" 2>&1 || rc=$?
[ "$rc" != 0 ] && [ "$(sha256sum "$scratch/reused/payload.sha256")" = "$before" ]
printf 'corruption\n' >> "$base/initramfs.cpio.gz"
rc=0
bash "$runner" "$scratch/kernel" "$scratch/corrupt" "$base" > "$scratch/corrupt.log" 2>&1 || rc=$?
[ "$rc" != 0 ] && [ "$calls" = "$(wc -l < "$OSITO_NESTED_FAKE_CALLS")" ]
cmp "$runner" "$scratch/corrupt/runner.sh"
[ -z "$(find "$scratch/disks/runs" -mindepth 1 -print -quit)" ]
echo 'PASS existing evidence unchanged, corrupt payload refused before launch, failed runner preserved'
# A fake executable would fail if image preparation tried to launch QEMU.
export QEMU_BIN="$scratch/fake-qemu" QEMU_PREPARE_ONLY=1
export QEMU_PID_FILE="$scratch/prepare.pid" QEMU_SERIAL_LOG="$scratch/serial.log"
bash "$script_dir/qemu-test-disks.sh" run -- bash "$script_dir/qemu-test.sh" \
    --no-build --no-gl --vnc > "$scratch/prepare.log" 2>&1
[ "$calls" = "$(wc -l < "$OSITO_NESTED_FAKE_CALLS")" ]
[ ! -e "$QEMU_PID_FILE" ] && [ ! -e "$QEMU_SERIAL_LOG" ]
grep -q 'Images prepared; QEMU was not started' "$scratch/prepare.log"
[ -z "$(find "$scratch/disks/runs" -mindepth 1 -print -quit)" ]
echo 'PASS real ESP preparation starts no QEMU and releases managed scratch'
