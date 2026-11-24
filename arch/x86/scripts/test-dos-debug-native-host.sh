#!/bin/bash
# Direct Linux KVM diagnosis. Does not boot or modify the Osito-K kernel.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
out=${OSITO_HOST_TEST_BUILD_DIR:-$repo/arch/x86/build/test-host/dos-debug-native}
mkdir -p "$out"
cc=${CC:-cc}
"$cc" -std=c11 -O2 -Wall -Wextra -Werror \
    "$repo/arch/x86/test/dos_debug_native_host.c" -o "$out/debug-host"
"$cc" -c "$repo/arch/x86/test/dos_debug_native_guest.S" -o "$out/guest.o"
objcopy -O binary -j .text "$out/guest.o" "$out/guest.bin"
exec "$out/debug-host" "$out/guest.bin"
