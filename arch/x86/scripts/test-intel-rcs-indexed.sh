#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required}
mkdir -p "$out"
ulimit -c 0
export ASAN_OPTIONS=halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
python3 arch/x86/lib/intel_d3d9/mesa-intel/src/intel/genxml/gen_pack_header.py --engines render \
    --include-symbols 3DSTATE_INDEX_BUFFER,3DPRIMITIVE \
    arch/x86/lib/intel_d3d9/mesa-intel/src/intel/genxml/gen90.xml > "$out/gen9-index-pack.h"
for cc in gcc clang; do
    for mode in normal sanitize; do
        flags=()
        [[ $mode == sanitize ]] && flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        "$cc" -std=gnu11 -O2 -g -Wall -Wextra -Werror "${flags[@]}" \
            -DHAVE_ENDIAN_H -I"$out" -Iarch/x86/lib/opengl/mesa/include \
            -Iarch/x86/lib/opengl/mesa/src -Iarch/x86/lib/intel_d3d9/mesa-intel/src/intel/genxml \
            arch/x86/test/intel_rcs_indexed_host.c arch/x86/drivers/intel_rcs_vertex.c \
            arch/x86/drivers/intel_rcs_vertex_test.c arch/x86/drivers/intel_rcs_sampled.c \
            arch/x86/drivers/intel_rcs_draw.c arch/x86/drivers/intel_gen9_texture.c \
            arch/x86/drivers/intel_gen9_shader.c -lm -o "$out/indexed-$cc-$mode"
        timeout 60 "$out/indexed-$cc-$mode" \
            arch/x86/build/native-sampler/host-qualified/ps30-tint.program \
            arch/x86/build/native-vertex-pipeline/fixtures/vs20-transform.program \
            arch/x86/build/native-vertex-pipeline/fixtures/vs30-transform.program \
            "$out/indexed-$cc-$mode.bin" | tee "$out/$cc-$mode.log"
    done
done
