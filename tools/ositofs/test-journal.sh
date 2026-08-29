#!/bin/sh
set -eu

TOOLS=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PY_SCRIPTS=$TOOLS/../../arch/x86/scripts
TMP_ROOT=${TMPDIR:-/tmp}/ositofs-journal-test.$$
BASE=$TMP_ROOT/base.img
trap 'rm -rf "$TMP_ROOT"' EXIT HUP INT TERM
mkdir -p "$TMP_ROOT"

repair_image()
{
    set +e
    "$TOOLS/ositofs-fsck" "$1" --repair >/dev/null
    status=$?
    set -e
    if [ "$status" -gt 1 ]; then
        echo "journal test: fsck repair failed with $status" >&2
        exit 1
    fi
}

truncate -s 32M "$BASE"
"$TOOLS/mkfs.ositofs" "$BASE" --label journal-test --block-size 65536 >/dev/null
"$TOOLS/ositofs-write" "$BASE" \
    "$TOOLS/../../include/common/ositofs2_format.h" --name alpha >/dev/null
"$TOOLS/ositofs-write" "$BASE" "$TOOLS/common.h" --name neighbor >/dev/null

for point in prepared committed entries primary-super backup-super; do
    image=$TMP_ROOT/$point.img
    cp "$BASE" "$image"
    set +e
    OSFS2_JOURNAL_FAILPOINT=$point \
        "$TOOLS/ositofs-rename" "$image" alpha beta >/dev/null 2>&1
    status=$?
    set -e
    if [ "$status" -ne 86 ]; then
        echo "journal test: failpoint $point exited $status, expected 86" >&2
        exit 1
    fi

    if [ "$point" = prepared ]; then
        repair_image "$image"
        "$TOOLS/ositofs-ls" "$image" | grep -q alpha
    else
        if "$TOOLS/ositofs-fsck" "$image" >/dev/null 2>&1; then
            echo "journal test: $point was not reported as pending" >&2
            exit 1
        fi
        repair_image "$image"
        "$TOOLS/ositofs-fsck" "$image" >/dev/null
        "$TOOLS/ositofs-ls" "$image" | grep -q beta
    fi
done

for point in prepared committed entries primary-super backup-super; do
    image=$TMP_ROOT/python-add-$point.img
    output=$TMP_ROOT/python-add-$point.out
    cp "$BASE" "$image"
    set +e
    OSFS2_JOURNAL_FAILPOINT=$point python3 "$PY_SCRIPTS/osfs2_write.py" \
        "$image" add "$TOOLS/common.c" gamma >/dev/null 2>&1
    status=$?
    set -e
    if [ "$status" -ne 86 ]; then
        echo "journal test: Python add failpoint $point exited $status, expected 86" >&2
        exit 1
    fi
    repair_image "$image"
    "$TOOLS/ositofs-fsck" "$image" >/dev/null
    if [ "$point" = prepared ]; then
        if "$TOOLS/ositofs-ls" "$image" | grep -q gamma; then
            echo "journal test: uncommitted Python add became visible" >&2
            exit 1
        fi
    else
        "$TOOLS/ositofs-read" "$image" gamma "$output" >/dev/null
        cmp "$output" "$TOOLS/common.c"
    fi
done

for point in prepared committed entries primary-super backup-super; do
    image=$TMP_ROOT/python-$point.img
    output=$TMP_ROOT/python-$point.out
    cp "$BASE" "$image"
    set +e
    OSFS2_JOURNAL_FAILPOINT=$point python3 "$PY_SCRIPTS/osfs2_replace.py" \
        "$image" "$TOOLS/common.c" alpha >/dev/null 2>&1
    status=$?
    set -e
    if [ "$status" -ne 86 ]; then
        echo "journal test: Python failpoint $point exited $status, expected 86" >&2
        exit 1
    fi
    repair_image "$image"
    "$TOOLS/ositofs-fsck" "$image" >/dev/null
    "$TOOLS/ositofs-read" "$image" alpha "$output" >/dev/null
    if [ "$point" = prepared ]; then
        cmp "$output" "$TOOLS/../../include/common/ositofs2_format.h"
    else
        cmp "$output" "$TOOLS/common.c"
    fi
done

for point in prepared committed entries primary-super backup-super; do
    image=$TMP_ROOT/replace-$point.img
    output=$TMP_ROOT/replace-$point.out
    cp "$BASE" "$image"
    set +e
    OSFS2_JOURNAL_FAILPOINT=$point \
        "$TOOLS/ositofs-write" "$image" "$TOOLS/common.c" \
        --name alpha --overwrite >/dev/null 2>&1
    status=$?
    set -e
    if [ "$status" -ne 86 ]; then
        echo "journal test: replace failpoint $point exited $status, expected 86" >&2
        exit 1
    fi
    repair_image "$image"
    "$TOOLS/ositofs-fsck" "$image" >/dev/null
    "$TOOLS/ositofs-read" "$image" alpha "$output" >/dev/null
    if [ "$point" = prepared ]; then
        cmp "$output" "$TOOLS/../../include/common/ositofs2_format.h"
    else
        cmp "$output" "$TOOLS/common.c"
    fi
done

image=$TMP_ROOT/delete.img
cp "$BASE" "$image"
set +e
OSFS2_JOURNAL_FAILPOINT=committed \
    "$TOOLS/ositofs-delete" "$image" alpha >/dev/null 2>&1
status=$?
set -e
if [ "$status" -ne 86 ]; then
    echo "journal test: delete failpoint exited $status, expected 86" >&2
    exit 1
fi
repair_image "$image"
"$TOOLS/ositofs-fsck" "$image" >/dev/null
if "$TOOLS/ositofs-ls" "$image" | grep -q alpha; then
    echo "journal test: committed delete was not replayed" >&2
    exit 1
fi

full=$TMP_ROOT/full.img
old_data=$TMP_ROOT/old-data.bin
extracted=$TMP_ROOT/extracted.bin
truncate -s 5M "$full"
dd if=/dev/zero of="$old_data" bs=65536 count=16 status=none
"$TOOLS/mkfs.ositofs" "$full" --block-size 65536 \
    --file-slots 4096 >/dev/null
"$TOOLS/ositofs-write" "$full" "$old_data" --name full >/dev/null
if "$TOOLS/ositofs-write" "$full" "$TOOLS/common.c" \
        --name full --overwrite >/dev/null 2>&1; then
    echo "journal test: no-space replacement unexpectedly succeeded" >&2
    exit 1
fi
"$TOOLS/ositofs-read" "$full" full "$extracted" >/dev/null
cmp "$old_data" "$extracted"
"$TOOLS/ositofs-fsck" "$full" >/dev/null

batch=$TMP_ROOT/batch.img
manifest=$TMP_ROOT/manifest.txt
cp "$BASE" "$batch"
printf '%s\t%s\n' "$TOOLS/common.c" delta > "$manifest"
python3 "$PY_SCRIPTS/osfs2_write_batch.py" "$batch" "$manifest" >/dev/null
"$TOOLS/ositofs-fsck" "$batch" >/dev/null
"$TOOLS/ositofs-read" "$batch" delta "$extracted" >/dev/null
cmp "$TOOLS/common.c" "$extracted"

grown=$TMP_ROOT/grown.img
cp "$BASE" "$grown"
python3 "$PY_SCRIPTS/osfs2_grow_image.py" "$grown" 600 >/dev/null
"$TOOLS/ositofs-fsck" "$grown" >/dev/null

maps_image=$TMP_ROOT/maps.img
maps_dir=$TMP_ROOT/maps
mkdir "$maps_dir"
cp "$TOOLS/common.c" "$maps_dir/test-map.unr"
cp "$BASE" "$maps_image"
python3 "$PY_SCRIPTS/osfs2_grow_addmaps.py" "$maps_image" "$maps_dir" 2 >/dev/null
"$TOOLS/ositofs-fsck" "$maps_image" >/dev/null
"$TOOLS/ositofs-read" "$maps_image" test-map.unr "$extracted" >/dev/null
cmp "$TOOLS/common.c" "$extracted"

echo "OSITOFS-JOURNAL-TEST: OK"
