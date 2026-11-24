#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-build/kprof-host}
mkdir -p "$out"
for cc in gcc clang; do
    for mode in normal sanitize; do
        flags=()
        if [[ $mode == sanitize ]]; then
            flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        fi
        "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror "${flags[@]}" \
            test/kprof_host.c -o "$out/kprof-$cc-$mode"
        "$out/kprof-$cc-$mode" | tee "$out/kprof-$cc-$mode.log"
    done
done
