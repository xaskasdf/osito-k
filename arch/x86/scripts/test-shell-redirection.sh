#!/bin/bash
set -euo pipefail
export UBSAN_OPTIONS=halt_on_error=1
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
repo=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$repo"
out=${1:-arch/x86/build/shell-redirection-host}
test ! -e "$out"
mkdir -p "$out"
cp --parents arch/x86/kernel/shell_redirect.h arch/x86/kernel/shell.c \
    arch/x86/fs/ositofs_metadata.h arch/x86/include/types.h \
    arch/x86/test/shell_redirect_host.c "$0" "$out/"
for compiler in gcc clang; do
    for mode in normal sanitize; do
        flags=()
        if [[ $mode == sanitize ]]; then
            flags=(-fsanitize=address,undefined -fno-omit-frame-pointer)
        fi
        "$compiler" -std=c11 -O2 -g -Wall -Wextra -Werror "${flags[@]}" \
            arch/x86/test/shell_redirect_host.c -o "$out/$compiler-$mode"
        "$out/$compiler-$mode" | tee "$out/$compiler-$mode.log"
    done
done
