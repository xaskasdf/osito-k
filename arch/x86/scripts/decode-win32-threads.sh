#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 1 ]; then
    echo "usage: $0 WIN32_THREADS_BIN [SLOT_COUNT]" >&2
    exit 2
fi

contexts=$1
slot_count=${2:-192}
record_size=${WIN32_THREAD_RECORD_SIZE:-20728}

read_u32() {
    od -An -tu4 -j "$2" -N4 "$1" | tr -d '[:space:]'
}

read_x32() {
    od -An -tx4 -j "$2" -N4 "$1" | tr -d '[:space:]'
}

read_x64() {
    od -An -tx8 -j "$2" -N8 "$1" | tr -d '[:space:]'
}

for ((slot = 0; slot < slot_count; slot++)); do
    off=$((slot * record_size))
    active=$(read_u32 "$contexts" "$off")
    if [ "$active" = 0 ]; then
        continue
    fi

    func=$(read_x64 "$contexts" "$((off + 8))")
    param=$(read_x64 "$contexts" "$((off + 16))")
    compat=$(read_u32 "$contexts" "$((off + 24))")
    tid=$(read_u32 "$contexts" "$((off + 28))")
    handle=$(read_x64 "$contexts" "$((off + 32))")
    terminated=$(read_u32 "$contexts" "$((off + 48))")
    exit_code=$(read_x32 "$contexts" "$((off + 52))")
    kernel_pid=$(read_u32 "$contexts" "$((off + 56))")
    suspended=$(read_u32 "$contexts" "$((off + 60))")
    stack_base=$(read_x64 "$contexts" "$((off + 0x1080))")
    stack_limit=$(read_x64 "$contexts" "$((off + 0x1088))")
    peb=$(read_x64 "$contexts" "$((off + 0x10d8))")

    printf 'slot=%03d pid=%-4s tid=%-4s arch=%s active=%s term=%s susp=%s func=%s param=%s handle=%s exit=%s stack=%s-%s peb=%s\n' \
        "$slot" "$kernel_pid" "$tid" "$([ "$compat" = 0 ] && echo 64 || echo 32)" \
        "$active" "$terminated" "$suspended" "$func" "$param" "$handle" \
        "$exit_code" "$stack_limit" "$stack_base" "$peb"
done
