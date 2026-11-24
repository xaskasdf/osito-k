#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-build/cpu-idle-host}
mkdir -p "$out"
for cc in gcc clang; do
    for mode in normal sanitize; do
        flags=()
        if [[ $mode == sanitize ]]; then
            flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        fi
        "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror "${flags[@]}" \
            test/cpu_idle_host.c -o "$out/cpu_idle-$cc-$mode"
        "$out/cpu_idle-$cc-$mode"
    done
done
