#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required}
compiler=${2:-clang}
mode=${3:-sanitize}
mkdir -p "$out"
flags=()
if [[ $mode == sanitize ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
for suite in transfer owner; do
    "$compiler" -std=c11 -O2 -g -fno-builtin -Wall -Wextra -Werror \
        -ffunction-sections -fdata-sections "${flags[@]}" \
        "arch/x86/test/http_${suite}_host.c" -Wl,--gc-sections \
        -o "$out/$suite-$compiler-$mode"
    "$out/$suite-$compiler-$mode" | tee "$out/$suite-$compiler-$mode.log"
done
