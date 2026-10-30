#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required};mkdir -p "$out"
for cc in gcc clang; do
    "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror -fPIC -shared -iquote arch/x86/include \
        -include arch/x86/test/variant_host_types.h arch/x86/test/imagehlp_host.c arch/x86/win32/imagehlp_shim.c \
        -o "$out/imagehlp-$cc.so"
    python3 arch/x86/scripts/test-imagehlp-oracle.py "$out/imagehlp-$cc.so"
done
