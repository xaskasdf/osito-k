#!/bin/bash
# Linux/WSL scratch disks. Persistent bases and evidence never live in runs/.
set -euo pipefail

usage() {
    echo "Usage: $0 run -- command [args...] | list | prune [--apply]"
    echo "OSITO_TEST_DISK_ROOT overrides ~/.local/share/osito-k/test-disks."
    echo "OSITO_KEEP_TEST_DISKS=1 retains scratch files until an explicit prune."
}

action=${1:-}
case "$action" in
    run) [ "${2:-}" = -- ] && [ "$#" -ge 3 ] || { usage; exit 2; } ;;
    list) [ "$#" = 1 ] || { usage; exit 2; } ;;
    prune) [ "$#" = 1 ] || { [ "$#" = 2 ] && [ "$2" = --apply ]; } || { usage; exit 2; } ;;
    *) usage; exit 2 ;;
esac
for tool in flock fuser realpath; do
    command -v "$tool" >/dev/null || { echo "Missing host tool: $tool" >&2; exit 1; }
done
umask 077
root=${OSITO_TEST_DISK_ROOT:-${XDG_DATA_HOME:-$HOME/.local/share}/osito-k/test-disks}
mkdir -p -- "$root"
root=$(realpath -e -- "$root")
mkdir -p -- "$root/runs" "$root/bases"
[ ! -L "$root/runs" ] || { echo "Refusing symlinked runs directory" >&2; exit 1; }
export OSITO_TEST_DISK_ROOT="$root"

managed() {
    local dir=$1 resolved
    [ -d "$dir" ] && [ ! -L "$dir" ] || return 1
    resolved=$(realpath -e -- "$dir") || return 1
    [ "${resolved%/*}" = "$root/runs" ] || return 1
    [[ ${resolved##*/} == run.* ]] || return 1
    [ -f "$dir/.osito-test-disks-v1" ] && [ ! -L "$dir/.osito-test-disks-v1" ] || return 1
    [ -f "$dir/.lease" ] && [ ! -L "$dir/.lease" ]
}

in_use() {
    local file
    # Also protect a child which closed inherited lease descriptors.
    while IFS= read -r -d '' file; do
        [ "${file##*/}" = .lease ] && continue
        if fuser -s "$file" 2>/dev/null; then return 0; fi
    done < <(find -P "$1" -xdev -type f -print0)
    return 1
}

remove_idle() (
    local dir=$1
    managed "$dir" || return 1
    exec 8>"$dir/.lease"
    flock -n 8 || return 1
    in_use "$dir" && return 1
    managed "$dir" || return 1
    rm -rf --one-file-system -- "$dir"
)

if [ "$action" != run ]; then
    while IFS= read -r -d '' dir; do
        managed "$dir" || continue
        state=idle
        if ! (exec 8>"$dir/.lease"; flock -n 8) || in_use "$dir"; then
            state=active
        fi
        printf '%s\t%s KiB\t%s\n' "$state" "$(du -sk -- "$dir" | cut -f1)" "$dir"
        if [ "$action" = prune ] && [ "${2:-}" = --apply ] && [ "$state" = idle ]; then
            remove_idle "$dir" || echo "Retained (became busy): $dir" >&2
        fi
    done < <(find -P "$root/runs" -mindepth 1 -maxdepth 1 -type d -name 'run.*' -print0)
    exit 0
fi

shift 2
dir=$(mktemp -d "$root/runs/run.XXXXXXXX")
exec 9>"$dir/.lease"
flock -n 9
touch "$dir/.osito-test-disks-v1"
child=

cleanup() {
    exec 9>&-
    if [ "${OSITO_KEEP_TEST_DISKS:-0}" = 1 ]; then
        echo "[test-disks] Retained: $dir" >&2
    elif ! remove_idle "$dir"; then
        echo "[test-disks] Still in use; prune after the child exits: $dir" >&2
    fi
}

interrupted() {
    trap '' INT TERM HUP
    if [ -n "$child" ]; then
        kill -TERM "$child" 2>/dev/null || true
        wait "$child" 2>/dev/null || true
    fi
    exit 130
}

trap cleanup EXIT
trap interrupted INT TERM HUP
export OSITO_TEST_DISK_DIR="$dir" QEMU_ESP_IMG="$dir/esp.img" TMPDIR="$dir"
echo "[test-disks] Scratch: $dir" >&2
# The foreground child must retain fd 9 and wait for its own QEMU process.
"$@" <&0 &
child=$!
rc=0
wait "$child" || rc=$?
exit "$rc"
