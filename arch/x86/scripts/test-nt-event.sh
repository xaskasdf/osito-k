#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:?output directory required}
mkdir -p "$out"
for cc in gcc clang; do
    for mode in normal sanitize; do
        flags=()
        if [[ $mode == sanitize ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
        "$cc" -O1 -g -ffunction-sections -fdata-sections -Iinclude "${flags[@]}" \
            test/nt_event_host.c -Wl,--gc-sections -o "$out/event-$cc-$mode"
        timeout 60 "$out/event-$cc-$mode" | tee "$out/$cc-$mode.log"
    done
done
