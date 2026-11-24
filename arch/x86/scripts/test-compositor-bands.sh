#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/tests/compositor-bands"
mkdir -p "$(dirname "$OUT")"
"${CC:-cc}" -std=gnu11 -O2 -ffunction-sections -fdata-sections \
    -I"$ROOT/include" -I"$ROOT/../../gui" "$ROOT/test/compositor_bands_test.c" \
    -Wl,--gc-sections -o "$OUT"
"$OUT"
