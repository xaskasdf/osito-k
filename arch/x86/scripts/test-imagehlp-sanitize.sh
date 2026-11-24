#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required};mkdir -p "$out"
for cc in gcc clang; do
    flags=()
    if [[ $cc == clang ]]; then
        flags=(-shared-libasan)
        asan_runtime="$(clang --print-runtime-dir)/libclang_rt.asan-x86_64.so"
    else asan_runtime=$(gcc -print-file-name=libasan.so);fi
    "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror -fPIC -shared -iquote arch/x86/include \
        -include arch/x86/test/variant_host_types.h -fsanitize=address,undefined -fno-omit-frame-pointer "${flags[@]}" \
        arch/x86/test/imagehlp_host.c arch/x86/win32/imagehlp_shim.c -o "$out/imagehlp-$cc.so"
    ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
        LD_PRELOAD="$asan_runtime" LD_LIBRARY_PATH="${asan_runtime%/*}:${LD_LIBRARY_PATH:-}" \
        python3 arch/x86/scripts/test-imagehlp-oracle.py "$out/imagehlp-$cc.so" | tee "$out/$cc.log"
done
