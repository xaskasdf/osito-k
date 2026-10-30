#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${1:-arch/x86/build/crt-startup}
cc=${CC:-clang}
mkdir -p "$out"
flags=(-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
for test in crt_args crt_startup; do
    "$cc" "${flags[@]}" "arch/x86/test/${test}_host.c" arch/x86/win32/crt_args.c -o "$out/$test-host"
    "$out/$test-host" | tee "$out/$test-host.log"
done
"$cc" "${flags[@]}" -ffunction-sections -fdata-sections -Iarch/x86/include \
    arch/x86/test/address_wait_host.c -Wl,--gc-sections -o "$out/address-wait-host"
"$out/address-wait-host" | tee "$out/address-wait-host.log"
"$cc" "${flags[@]}" arch/x86/test/crt_exit_host.c -o "$out/crt-exit-host"
"$out/crt-exit-host" | tee "$out/crt-exit-host.log"
