#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-build/thermal-host}
mkdir -p "$out"
for cc in gcc clang; do
    for mode in normal sanitize; do
        flags=()
        if [[ $mode == sanitize ]]; then
            flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        fi
        for test in thermal_host thermal_sample_host; do
            "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror "${flags[@]}" \
                "test/$test.c" -o "$out/$test-$cc-$mode"
            "$out/$test-$cc-$mode"
        done
    done
done
