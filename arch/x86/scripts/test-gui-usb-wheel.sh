#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:-build/gui-usb-wheel}
test ! -e "$out"
mkdir -p "$out"
for compiler in gcc clang; do
    for mode in normal sanitize; do
        flags=(-std=c11 -O2 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections)
        if [[ $mode == sanitize ]]; then
            flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        fi
        for bridge in present absent; do
            extra=()
            [[ $bridge == present ]] || extra+=(-DOMIT_WIN32_BRIDGE)
            name="$compiler-$mode-$bridge"
            "$compiler" "${flags[@]}" "${extra[@]}" -Iarch/x86/include -Iinclude/common \
                arch/x86/test/gui_usb_wheel_host.c arch/x86/drivers/hid_parser.c \
                -Wl,--gc-sections -o "$out/$name"
            "$out/$name" | tee "$out/$name.log"
        done
    done
done
