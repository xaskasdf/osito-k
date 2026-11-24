#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:?output directory required}
mkdir -p "$out"
ulimit -c 0
for cc in gcc clang; do
    for mode in normal sanitize; do
        flags=()
        [[ $mode == sanitize ]] && flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
        "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${flags[@]}" \
            test/display_source_host.c -o "$out/source-$cc-$mode"
        timeout 60 "$out/source-$cc-$mode" | tee "$out/$cc-$mode.log"
    done
done
