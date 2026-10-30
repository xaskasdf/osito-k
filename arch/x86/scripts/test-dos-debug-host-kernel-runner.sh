#!/bin/bash
# Test runner status/provenance handling without starting a VM.
set -euo pipefail
if [ "${OSITO_KVM_RUNNER_FAKE:-0}" = 1 ]; then
    if [ "${1:-}" = --version ]; then echo 'fake QEMU for runner tests'; exit 0; fi
    serial=
    while [ "$#" -gt 0 ]; do
        if [ "$1" = -serial ]; then serial=${2#file:}; shift; fi
        shift
    done
    [ -n "$serial" ]
    case ${OSITO_KVM_RUNNER_RESULT:-pass} in
        pass) printf '[KVM-HOST-BOOT] probe-exit=0\n' > "$serial" ;;
        fail) printf '[KVM-HOST-BOOT] probe-exit=1\n' > "$serial" ;;
        duplicate) printf '[KVM-HOST-BOOT] probe-exit=0\n[KVM-HOST-BOOT] probe-exit=1\n' > "$serial" ;;
        missing) printf 'boot did not finish\n' > "$serial" ;;
        qemu-fail) exit 42 ;;
        timeout) exit 124 ;;
        *) exit 2 ;;
    esac
    exit 0
fi
script_dir=$(cd "$(dirname "$0")" && pwd)
runner=$script_dir/test-dos-debug-host-kernel.sh
scratch=$(mktemp -d /tmp/osito-kvm-runner.XXXXXXXX)
cleanup() {
    [ "$(realpath -e "$scratch")" = "$scratch" ] && [[ "$scratch" == /tmp/osito-kvm-runner.* ]] || return 1
    rm -rf --one-file-system -- "$scratch"
}
trap cleanup EXIT
cp "$0" "$scratch/fake-qemu"
chmod +x "$scratch/fake-qemu"
export QEMU_BIN="$scratch/fake-qemu" OSITO_KVM_RUNNER_FAKE=1
printf 'test kernel payload\n' > "$scratch/kernel"
rc=0
bash "$runner" > "$scratch/usage.log" 2>&1 || rc=$?
[ "$rc" = 2 ]
bash "$runner" "$scratch/kernel" "$scratch/base with spaces" > "$scratch/base.log" 2>&1
base="$scratch/base with spaces"
[ "$(cat "$base/probe.exit")" = 0 ]
OSITO_KVM_TEST_KERNEL_ARGS='kvm_amd.nrips=0' bash "$runner" "$scratch/kernel" \
    "$scratch/reused" "$base" > "$scratch/reused.log" 2>&1
cmp "$base/initramfs.cpio.gz" "$scratch/reused/initramfs.cpio.gz"
cmp "$base/root/debug-host" "$scratch/reused/root/debug-host"
cmp "$base/sources.sha256" "$scratch/reused/sources.sha256"
grep -q 'kvm_amd.nrips=0' "$scratch/reused/qemu-command.txt"
[ "$(cat "$scratch/reused/payload-source.txt")" = "$base" ]
echo 'PASS new payload, byte-identical reuse, source provenance, spaces, kernel arguments'
(cd "$script_dir/.."; bash scripts/test-dos-debug-host-kernel.sh "$scratch/kernel" \
    "$scratch/relative" "$base") > "$scratch/relative.log" 2>&1
cmp "$runner" "$scratch/relative/runner.sh"
echo 'PASS relative invocation outside the repository root'
for case in fail duplicate missing qemu-fail timeout; do
    rc=0
    OSITO_KVM_RUNNER_RESULT=$case bash "$runner" "$scratch/kernel" "$scratch/$case" \
        "$base" > "$scratch/$case.log" 2>&1 || rc=$?
    case $case in
        fail) [ "$rc" = 1 ] && [ "$(cat "$scratch/$case/probe.exit")" = 1 ] ;;
        duplicate|missing) [ "$rc" = 125 ] && [ ! -e "$scratch/$case/probe.exit" ] ;;
        qemu-fail) [ "$rc" = 42 ] && [ "$(cat "$scratch/$case/qemu.exit")" = 42 ] ;;
        timeout) [ "$rc" = 124 ] && [ "$(cat "$scratch/$case/qemu.exit")" = 124 ] ;;
    esac
done
echo 'PASS probe failure, absent/duplicate completion, QEMU failure and timeout statuses'
before=$(sha256sum "$base/binaries.sha256")
rc=0
bash "$runner" "$scratch/kernel" "$base" > "$scratch/existing.log" 2>&1 || rc=$?
[ "$rc" != 0 ] && [ "$(sha256sum "$base/binaries.sha256")" = "$before" ]
printf 'corrupt payload\n' >> "$base/initramfs.cpio.gz"
rc=0
bash "$runner" "$scratch/kernel" "$scratch/corrupt" "$base" > "$scratch/corrupt.log" 2>&1 || rc=$?
[ "$rc" != 0 ] && [ ! -e "$scratch/corrupt" ]
echo 'PASS existing evidence preservation and corrupt payload rejection before boot'
