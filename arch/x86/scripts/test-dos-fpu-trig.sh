#!/bin/bash
# MPFR is a host-only oracle, never a kernel dependency.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
log=${1:?provide a new output log path}
test ! -e "$log"
build="$repo/arch/x86/build/fpu-trig-host-test"
mkdir -p "$build"
exec 9>"$build/.lock"
flock -n 9 || { echo "Trig host test is already running" >&2; exit 1; }
cc=${HOST_CC:-clang}
cxx=${HOST_CXX:-clang++}
common=(-O2 -Wall -Wextra -Werror -fsanitize=address,undefined)
cxxflags=(-fno-exceptions -fno-rtti -I"$repo/arch/x86/lib"
    -I"$repo/arch/x86/lib/softfloat3e" -I"$repo/arch/x86/lib/softfloat3e/include")
{
"$cc" "${common[@]}" -c "$repo/arch/x86/test/dos_fpu_trig_host.c" -o "$build/test.o"
"$cxx" "${common[@]}" "${cxxflags[@]}" \
    -c "$repo/arch/x86/dos/dos_fpu_softfloat.cc" -o "$build/adapter.o"
objects=()
for source in "$repo/arch/x86/lib/softfloat3e/"*.cc \
    "$repo/arch/x86/lib/softfloat3e/8086-SSE/"*.cc \
    "$repo/arch/x86/lib/bochs-transcendental/"*.cc; do
    object="$build/vendor-$(basename "$source" .cc).o"
    "$cxx" "${common[@]}" "${cxxflags[@]}" -Wno-error=implicit-fallthrough \
        -c "$source" -o "$object"
    objects+=("$object")
done
"$cxx" "${common[@]}" "$build/test.o" "$build/adapter.o" "${objects[@]}" \
    -lmpfr -lgmp -o "$build/test"
"$build/test"
} 2>&1 | tee "$log"
grep -Eq '^TRIG MPFR inputs=[1-9][0-9]* cases=[1-9][0-9]* checks=[1-9][0-9]* exact=[1-9][0-9]* approximate=[1-9][0-9]* failures=0$' "$log"
