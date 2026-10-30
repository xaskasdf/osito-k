#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:?output required}
mkdir -p "$out"
ulimit -c 0
for cc in gcc clang; do
    for mode in normal sanitize; do
        flags=()
        if [[ $mode == sanitize ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
        "$cc" -O2 -g -Wall -Wextra -Werror -ffunction-sections -Wl,--gc-sections "${flags[@]}" \
            test/intel_fence_host.c drivers/intel_ppgtt.c drivers/intel_bcs.c -o "$out/fence-$cc-$mode"
        timeout 60 "$out/fence-$cc-$mode" | tee "$out/$cc-$mode.log"
    done
done
