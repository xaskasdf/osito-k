#!/bin/sh
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
x86_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
out_dir=${1:-"$x86_dir/build/test"}
mkdir -p "$out_dir"
"${CC:-gcc}" -O2 -ffreestanding -fno-builtin -fno-stack-protector \
    -fno-pie -no-pie -nostdlib -static -Wl,--build-id=none \
    "$x86_dir/test/tls_segments.c" -o "$out_dir/tls_segments"
printf '%s\n' "$out_dir/tls_segments"
