#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 2 ]; then
    echo "usage: $0 PROCTAB_BIN SCHED_BIN [SLOT_COUNT]" >&2
    exit 2
fi

proctab=$1
sched=$2
slot_count=${3:-256}
record_size=${PROCESS_RECORD_SIZE:-1776}

read_u32() {
    od -An -tu4 -j "$2" -N4 "$1" | tr -d '[:space:]'
}

read_u64() {
    od -An -tu8 -j "$2" -N8 "$1" | tr -d '[:space:]'
}

read_x64() {
    od -An -tx8 -j "$2" -N8 "$1" | tr -d '[:space:]'
}

for ((slot = 0; slot < slot_count; slot++)); do
    process_off=$((slot * record_size))
    pid=$(read_u32 "$proctab" "$process_off")
    state=$(read_u32 "$proctab" "$((process_off + 8))")
    if [ "$state" = 0 ]; then
        continue
    fi

    name=$(dd if="$proctab" bs=1 skip="$((process_off + 12))" count=32 \
        status=none | tr -d '\000')
    seq=$(read_u64 "$sched" "$((slot * 8))")
    rsp=$(read_x64 "$sched" "$((0x800 + slot * 8))")
    qword0=$(read_x64 "$sched" "$((0x1000 + slot * 8))")
    qword1=$(read_x64 "$sched" "$((0x1800 + slot * 8))")
    resumed=$(read_u64 "$sched" "$((0x2000 + slot * 8))")

    printf 'slot=%03d pid=%-4s state=%s seq=%s/%s rsp=%s q=%s/%s name=%s\n' \
        "$slot" "$pid" "$state" "$seq" "$resumed" "$rsp" \
        "$qword0" "$qword1" "$name"
done
