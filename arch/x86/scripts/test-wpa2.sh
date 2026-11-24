#!/bin/bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$repo"
out=${1:-arch/x86/build/wpa2-host}
test ! -e "$out"
mkdir -p "$out"
cp --parents arch/x86/kernel/wpa2{,_crypto,_rsn,_selftest}.{c,h} arch/x86/kernel/wpa2_status.h arch/x86/kernel/random_secure.{c,h} \
    arch/x86/test/wpa2_host.c "$0" "$out/"
cp -r --parents arch/x86/lib/bearssl "$out/"
sources=(arch/x86/kernel/{wpa2,wpa2_crypto,wpa2_rsn,wpa2_selftest,random_secure}.c
    arch/x86/lib/bearssl/src/hash/sha1.c arch/x86/lib/bearssl/src/mac/hmac.c
    arch/x86/lib/bearssl/src/codec/{dec32be,enc32be}.c
    arch/x86/lib/bearssl/src/symcipher/{aes_ct,aes_ct_dec,aes_ct_cbcdec}.c)
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1
out=$(realpath "$out")
cd "$out"
for cc in gcc clang; do
    for mode in normal sanitize; do
        extra=()
        if [[ $mode == sanitize ]]; then extra=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
        "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror "${extra[@]}" \
            -DRANDOM_SECURE_HOST_TEST -DBR_LE_UNALIGNED=0 -DBR_BE_UNALIGNED=0 \
            -Iarch/x86/lib/bearssl/inc -Iarch/x86/lib/bearssl/src \
            arch/x86/test/wpa2_host.c "${sources[@]}" -lcrypto -o "$out/wpa2-$cc-$mode"
        "$out/wpa2-$cc-$mode" | tee "$out/wpa2-$cc-$mode.log"
    done
done
