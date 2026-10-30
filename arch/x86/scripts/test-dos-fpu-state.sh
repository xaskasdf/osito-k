#!/bin/bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
log=${1:?provide a new output log path}
test ! -e "$log"
build="$repo/arch/x86/build/fpu-state-host-test"
mkdir -p "$build"
exec 9>"$build/.lock"
flock -n 9 || { echo 'State host test is already running' >&2; exit 1; }
cc=${HOST_CC:-clang}
cxx=${HOST_CXX:-clang++}
common=(-O2 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all)
{
"$cc" "${common[@]}" -c "$repo/arch/x86/test/dos_fpu_state_host.c" -o "$build/test.o"
"$cc" -DDOS_FPU_HOST_TEST -c "$repo/arch/x86/dos/dos_fpu_host.S" -o "$build/host.o"
"$cxx" "${common[@]}" -fno-exceptions -fno-rtti -c "$repo/arch/x86/dos/dos_fpu_state.cc" -o "$build/state.o"
"$cxx" "${common[@]}" "$build/test.o" "$build/host.o" "$build/state.o" -lmpfr -lgmp -o "$build/test"
"$build/test"
} 2>&1 | tee "$log"
grep -Eq '^STATE HOST forms=128 cases=[1-9][0-9]* checks=[1-9][0-9]* failures=0$' "$log"
