#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required}
mkdir -p "$out"
ulimit -c 0
python3 arch/x86/lib/intel_d3d9/mesa-intel/src/intel/genxml/gen_pack_header.py \
    --engines render --include-symbols RENDER_SURFACE_STATE \
    arch/x86/lib/intel_d3d9/mesa-intel/src/intel/genxml/gen90.xml > "$out/gen9-copy-pack.h"
for cc in gcc clang; do
    for mode in normal sanitize; do
        flags=()
        [[ $mode == sanitize ]] && flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        "$cc" -std=gnu11 -O2 -g -Wall -Wextra -Werror "${flags[@]}" \
            -DHAVE_ENDIAN_H -I"$out" -Iarch/x86/lib/opengl/mesa/include \
            -Iarch/x86/lib/opengl/mesa/src -Iarch/x86/lib/intel_d3d9/mesa-intel/src/intel/genxml \
            arch/x86/test/intel_copy_rect_host.c arch/x86/drivers/intel_bcs.c \
            arch/x86/drivers/intel_cs.c arch/x86/drivers/intel_rcs_draw.c arch/x86/drivers/intel_rcs.c \
            arch/x86/drivers/intel_gen9_texture.c -lm -o "$out/copy-$cc-$mode"
        timeout 60 "$out/copy-$cc-$mode" | tee "$out/$cc-$mode.log"
    done
done
