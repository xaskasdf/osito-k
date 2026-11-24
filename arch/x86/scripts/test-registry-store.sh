#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-build/registry-store-host}
mkdir -p "$out"
for cc in gcc clang; do
    for mode in normal ubsan; do
        flags=()
        if [[ $mode == ubsan ]]; then flags+=(-fsanitize=undefined -fno-sanitize-recover=all); fi
        source=()
        if [[ -n ${REGISTRY_TEST_SOURCE:-} ]]; then
            source+=("-DREGISTRY_TEST_SOURCE=\"$(realpath "$REGISTRY_TEST_SOURCE")\"")
        fi
        "$cc" -std=c11 -O2 -g -Wall -Wextra "${flags[@]}" \
            -c test/registry_context_host.c -o "$out/context-$cc-$mode.o"
        "$cc" -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter \
            -ffunction-sections -fdata-sections -iquote include -iquote win32 -iquote ../../include/common \
            "${flags[@]}" "${source[@]}" test/registry_store_host.c \
            "$out/context-$cc-$mode.o" -Wl,--gc-sections -o "$out/registry-$cc-$mode"
        for test in contention init-only; do
            args=()
            if [[ $test == init-only ]]; then args+=(init-only); fi
            if [[ ${REGISTRY_TEST_EXPECT_STALL:-0} == 1 ]]; then
                status=0
                timeout 2 "$out/registry-$cc-$mode" "${args[@]}" || status=$?
                test "$status" = 124
                printf 'Confirmed baseline stall: %s %s %s\n' "$cc" "$mode" "$test"
            else
                timeout 10 "$out/registry-$cc-$mode" "${args[@]}"
            fi
        done
    done
done
