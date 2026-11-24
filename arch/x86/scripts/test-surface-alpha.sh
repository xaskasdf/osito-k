#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-build/surface-alpha-host}
mkdir -p "$out"
flags=(-std=c11 -O2 -g -Wall -Wextra -Werror)
if [[ ${SANITIZE:-0} == 1 ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
"${CC:-cc}" "${flags[@]}" test/surface_alpha_host.c -o "$out/surface-alpha-host"
"$out/surface-alpha-host"
