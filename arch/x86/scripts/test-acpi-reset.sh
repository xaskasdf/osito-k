#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-build/acpi-reset-host}
mkdir -p "$out"
for cc in gcc clang; do
    for mode in normal sanitize; do
        flags=()
        if [[ $mode == sanitize ]]; then
            flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        fi
        for suite in acpi_reset pci_wlan pci_dma; do
            "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror "${flags[@]}" \
                -ffunction-sections -fdata-sections -Wl,--gc-sections \
                "test/${suite}_host.c" -o "$out/$suite-$cc-$mode"
            "$out/$suite-$cc-$mode"
        done
    done
done
