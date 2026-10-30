#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:?output directory required}
mkdir -p "$out"
clang --target=x86_64-linux-gnu -O2 -ffreestanding -fno-stack-protector \
    -mgeneral-regs-only -mno-red-zone -fno-pie -nostdlib -static -Wl,-e,_start \
    arch/x86/test/shm_lifetime_elf.c -o "$out/shm-lifetime.elf"
