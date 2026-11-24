#!/bin/sh
set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "usage: inject-kernel-journal-test.sh <image> [source-file]" >&2
    exit 1
fi

TOOLS=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
IMAGE=$1
SOURCE=${2:-$TOOLS/../../include/common/ositofs2_format.h}
OLD=journal-kernel-before
NEW=journal-kernel-after

"$TOOLS/ositofs-delete" "$IMAGE" "$OLD" >/dev/null 2>&1 || true
"$TOOLS/ositofs-delete" "$IMAGE" "$NEW" >/dev/null 2>&1 || true
"$TOOLS/ositofs-write" "$IMAGE" "$SOURCE" --name "$OLD" >/dev/null

set +e
OSFS2_JOURNAL_FAILPOINT=committed \
    "$TOOLS/ositofs-rename" "$IMAGE" "$OLD" "$NEW" >/dev/null 2>&1
status=$?
set -e
if [ "$status" -ne 86 ]; then
    echo "kernel journal injection exited $status, expected 86" >&2
    exit 1
fi
