#!/bin/bash
# Independent MPFR conversions and isolated native memory/stack checks.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
log=${1:?provide a new output log path}
test ! -e "$log"
build="$repo/arch/x86/build/fpu-convert-host-test"
mkdir -p "$build"
exec 9>"$build/.lock"
flock -n 9 || { echo 'Conversion host test is already running' >&2; exit 1; }
cc=${HOST_CC:-clang}
cxx=${HOST_CXX:-clang++}
common=(-O2 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all)
cxxflags=(-fno-exceptions -fno-rtti -I"$repo/arch/x86/lib"
    -I"$repo/arch/x86/lib/softfloat3e" -I"$repo/arch/x86/lib/softfloat3e/include")
{
"$cc" "${common[@]}" -c "$repo/arch/x86/test/dos_fpu_convert_host.c" -o "$build/test.o"
"$cc" -DDOS_FPU_HOST_TEST -c "$repo/arch/x86/dos/dos_fpu_host.S" -o "$build/host.o"
"$cxx" "${common[@]}" "${cxxflags[@]}" -c "$repo/arch/x86/dos/dos_fpu_softfloat.cc" -o "$build/adapter.o"
objects=()
for source in "$repo/arch/x86/lib/softfloat3e/"*.cc \
    "$repo/arch/x86/lib/softfloat3e/8086-SSE/"*.cc "$repo/arch/x86/lib/bochs-transcendental/"*.cc; do
    object="$build/vendor-$(basename "$source" .cc).o"
    "$cxx" "${common[@]}" "${cxxflags[@]}" -Wno-error=implicit-fallthrough -c "$source" -o "$object"
    objects+=("$object")
done
"$cxx" "${common[@]}" "$build/test.o" "$build/host.o" "$build/adapter.o" "${objects[@]}" \
    -lmpfr -lgmp -o "$build/test"
"$build/test"
} 2>&1 | tee "$log"
grep -Eq '^CONVERT MPFR forms=21 inputs=[1-9][0-9]* evaluations=[1-9][0-9]* checks=[1-9][0-9]* native=[1-9][0-9]* failures=0$' "$log"
