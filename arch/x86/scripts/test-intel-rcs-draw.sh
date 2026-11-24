#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-build/intel-rcs-draw-host}
mkdir -p "$out"
ulimit -c 0
for cc in gcc clang; do
 for mode in normal sanitize; do
  flags=()
  if [[ $mode == sanitize ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
  "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror "${flags[@]}" \
   test/intel_rcs_draw_host.c drivers/intel_rcs_draw.c drivers/intel_rcs.c \
   -o "$out/draw-$cc-$mode"
  "$out/draw-$cc-$mode" | tee "$out/draw-$cc-$mode.log"
 done
done
