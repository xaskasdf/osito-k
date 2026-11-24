#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required}
mkdir -p "$out"
ulimit -c 0
export ASAN_OPTIONS=halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
python3 arch/x86/scripts/generate-gen9-vertex-reference.py "$out"
for cc in gcc clang; do
    for mode in normal sanitize; do
        flags=()
        [[ $mode == sanitize ]] && flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        "$cc" -std=gnu11 -O2 -g -Wall -Wextra -Werror "${flags[@]}" \
            -DHAVE_ENDIAN_H -I"$out" -Iarch/x86/lib/opengl/mesa/include \
            -Iarch/x86/lib/opengl/mesa/src -Iarch/x86/lib/intel_d3d9/mesa-intel/src/intel/genxml \
            arch/x86/test/intel_rcs_vertex_host.c arch/x86/drivers/intel_rcs_vertex.c \
            arch/x86/drivers/intel_rcs_sampled.c arch/x86/drivers/intel_rcs_draw.c \
            arch/x86/drivers/intel_gen9_texture.c arch/x86/drivers/intel_gen9_shader.c \
            -lm -o "$out/vertex-$cc-$mode"
        timeout 60 "$out/vertex-$cc-$mode" \
            arch/x86/build/native-vertex-shader/host-final/vs30-outputs.program \
            arch/x86/build/native-sampler/host-qualified/ps30-tint.program | tee "$out/$cc-$mode.log"
    done
done
