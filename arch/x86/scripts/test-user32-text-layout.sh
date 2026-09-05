#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/tests/user32-text-layout"
mkdir -p "$(dirname "$OUT")"
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror \
    -include stdbool.h -fsanitize=address,undefined -fno-omit-frame-pointer \
    "$ROOT/test/user32_text_layout_test.c" "$ROOT/win32/user32_text.c" -o "$OUT"
"$OUT"
