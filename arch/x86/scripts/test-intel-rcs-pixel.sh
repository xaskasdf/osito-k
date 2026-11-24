#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-build/intel-rcs-pixel-host}
mkdir -p "$out"
ulimit -c 0
for cc in gcc clang; do
 for mode in normal sanitize; do
  flags=()
  if [[ $mode == sanitize ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
  "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror "${flags[@]}" \
   test/intel_rcs_pixel_host.c drivers/intel_rcs_draw.c -o "$out/pixel-$cc-$mode"
  UBSAN_OPTIONS=halt_on_error=1 ASAN_OPTIONS=detect_leaks=1 \
   "$out/pixel-$cc-$mode" | tee "$out/pixel-$cc-$mode.log"
 done
done
