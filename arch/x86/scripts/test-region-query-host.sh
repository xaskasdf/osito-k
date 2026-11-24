#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:?output directory};mkdir -p "$out"
flags=(-std=c11 -O2 -g -fno-builtin -DGDI_HOST_TEST=1 -Wall -Wextra -Wno-unused-parameter -Wno-address
 -ffunction-sections -fdata-sections -Iinclude -Iwin32 -Ikernel -I../../include/common)
for compiler in gcc clang;do
 for mode in normal sanitize;do
  extra=();[[ $mode == sanitize ]] && extra+=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  for test in region_query gdi_region_dynamic;do
   binary="$out/$test-$compiler-$mode"
   "$compiler" "${flags[@]}" "${extra[@]}" "test/${test}_host.c" test/gdi_unreached_host.c -Wl,--gc-sections,--wrap=malloc,--wrap=free -o "$binary"
   "$binary"
  done
 done
done
