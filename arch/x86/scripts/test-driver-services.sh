#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-build/driver-services-host}
mkdir -p "$out"
for compiler in gcc clang; do
    for mode in normal sanitize; do
        flags=()
        if [[ $mode == sanitize ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
        "$compiler" -std=c11 -O2 -g -Wall -Wextra -Werror -Iinclude \
            -ffunction-sections -fdata-sections "${flags[@]}" \
            test/driver_services_host.c kernel/driver_manager.c kernel/wlan_profiles.c kernel/wpa2_rsn.c \
            -Wl,--gc-sections -o "$out/$compiler-$mode"
        "$out/$compiler-$mode"
        "$compiler" -std=c11 -O2 -g -Wall -Wextra -Werror -Iinclude \
            -ffunction-sections -fdata-sections "${flags[@]}" \
            test/driver_store_host.c kernel/crypto.c -Wl,--gc-sections -o "$out/store-$compiler-$mode"
        "$out/store-$compiler-$mode"
        "$compiler" -std=c11 -O2 -g -Wall -Wextra -Werror -Iinclude \
            -ffunction-sections -fdata-sections "${flags[@]}" \
            test/driver_firmware_host.c kernel/crypto.c -Wl,--gc-sections -o "$out/firmware-$compiler-$mode"
        "$out/firmware-$compiler-$mode"
        "$compiler" -std=c11 -O2 -g -Wall -Wextra -Werror -Iinclude \
            -ffunction-sections -fdata-sections "${flags[@]}" \
            test/http_owner_host.c -Wl,--gc-sections -o "$out/http-$compiler-$mode"
        "$out/http-$compiler-$mode"
    done
done
