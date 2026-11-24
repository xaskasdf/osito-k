#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/tests/spec-prefetch"
mkdir -p "$(dirname "$OUT")"
"${CC:-cc}" -std=gnu11 -O2 "$ROOT/test/spec_prefetch_test.c" -o "$OUT"
"$OUT"
