#!/bin/bash
set -euo pipefail
script_dir=$(cd "$(dirname "$0")" && pwd)
manager="$script_dir/qemu-test-disks.sh"
sandbox=$(mktemp -d)
wrapper=
cleanup() {
    if [ -n "$wrapper" ]; then
        kill -TERM "$wrapper" 2>/dev/null || true
        wait "$wrapper" 2>/dev/null || true
    fi
    if [ -s "$sandbox/evidence/child.pid" ]; then
        kill "$(cat "$sandbox/evidence/child.pid")" 2>/dev/null || true
    fi
    rm -rf --one-file-system -- "$sandbox"
}
trap cleanup EXIT
export OSITO_TEST_DISK_ROOT="$sandbox/disk pool"
export TEST_EVIDENCE="$sandbox/evidence"
mkdir "$TEST_EVIDENCE"

empty_runs() {
    [ -z "$(find "$OSITO_TEST_DISK_ROOT/runs" -mindepth 1 -maxdepth 1 -type d -print -quit)" ]
}

bash "$manager" run -- bash -c '
    test "$TMPDIR" = "$OSITO_TEST_DISK_DIR"
    test "$QEMU_ESP_IMG" = "$OSITO_TEST_DISK_DIR/esp.img"
    truncate -s 128M "$OSITO_TEST_DISK_DIR/disk.img"
    echo evidence > "$TEST_EVIDENCE/serial.log"
'
empty_runs
test -s "$TEST_EVIDENCE/serial.log"
rc=0
bash "$manager" run -- bash -c 'touch "$QEMU_ESP_IMG"; exit 42' || rc=$?
test "$rc" = 42
empty_runs
echo 'PASS normal exit, failure status, spaces, evidence retention'

OSITO_KEEP_TEST_DISKS=1 bash "$manager" run -- touch "$sandbox/keep-returned"
test -e "$sandbox/keep-returned"
test "$(bash "$manager" list | cut -f1)" = idle
bash "$manager" prune
test -n "$(bash "$manager" list)"
bash "$manager" prune --apply
empty_runs
echo 'PASS retained images, dry run, explicit prune'

# A detached child retains the lease after the wrapper itself has exited.
bash "$manager" run -- bash -c '
    sleep 120 < /dev/null > /dev/null 2>&1 &
    echo $! > "$TEST_EVIDENCE/child.pid"
'
test "$(bash "$manager" list | cut -f1)" = active
bash "$manager" prune --apply
test "$(bash "$manager" list | cut -f1)" = active
kill "$(cat "$TEST_EVIDENCE/child.pid")"
rm "$TEST_EVIDENCE/child.pid"
for attempt in {1..100}; do
    [ "$(bash "$manager" list | cut -f1)" = idle ] && break
    sleep 0.02
done
bash "$manager" prune --apply
empty_runs
echo 'PASS orphan child lease blocks deletion'

# Open files also protect children which deliberately close the lease.
bash "$manager" run -- bash -c '
    touch "$QEMU_ESP_IMG"
    sleep 120 9>&- < "$QEMU_ESP_IMG" > /dev/null 2>&1 &
    echo $! > "$TEST_EVIDENCE/child.pid"
'
test "$(bash "$manager" list | cut -f1)" = active
bash "$manager" prune --apply
kill "$(cat "$TEST_EVIDENCE/child.pid")"
rm "$TEST_EVIDENCE/child.pid"
for attempt in {1..100}; do
    [ "$(bash "$manager" list | cut -f1)" = idle ] && break
    sleep 0.02
done
bash "$manager" prune --apply
empty_runs
echo 'PASS open image blocks deletion without inherited lease'

# Never traverse an unowned folder or a symlink out of the managed tree.
mkdir "$OSITO_TEST_DISK_ROOT/runs/run.unowned" "$sandbox/outside"
touch "$sandbox/outside/sentinel"
ln -s "$sandbox/outside" "$OSITO_TEST_DISK_ROOT/runs/run.symlink"
bash "$manager" prune --apply
test -d "$OSITO_TEST_DISK_ROOT/runs/run.unowned"
test -e "$sandbox/outside/sentinel"
ln -s "$sandbox/outside/sentinel" "$OSITO_TEST_DISK_ROOT/bases/sentinel"
bash "$manager" prune --apply
test -L "$OSITO_TEST_DISK_ROOT/bases/sentinel"
echo 'PASS path guards and persistent base exclusion'

OSITO_TEST_DISK_ROOT="$sandbox/signal" bash "$manager" run -- bash -c '
    echo ready > "$TEST_EVIDENCE/signal-ready"
    exec sleep 120
' &
wrapper=$!
for attempt in {1..100}; do
    [ -s "$TEST_EVIDENCE/signal-ready" ] && break
    sleep 0.02
done
test -s "$TEST_EVIDENCE/signal-ready"
kill -TERM "$wrapper"
rc=0
wait "$wrapper" || rc=$?
wrapper=
test "$rc" = 130
OSITO_TEST_DISK_ROOT="$sandbox/signal" empty_runs
echo 'PASS signal forwarding and cleanup after child exit'
