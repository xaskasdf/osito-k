#!/usr/bin/env bash
# ECDHE on P-256 and P-384 (kernel/ecdh.c) against BearSSL's EC code, with
# gcc and clang, with and without ASan/UBSan
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-build/ecdh-host}
mkdir -p "$out"
sources=(test/ecdh_host.c kernel/ecdh.c lib/bearssl/src/ec/ec_prime_i31.c
         lib/bearssl/src/ec/ec_secp256r1.c lib/bearssl/src/ec/ec_secp384r1.c
         lib/bearssl/src/ec/ec_secp521r1.c lib/bearssl/src/codec/ccopy.c
         lib/bearssl/src/codec/dec32be.c lib/bearssl/src/codec/enc32be.c
         lib/bearssl/src/int/*.c)
for compiler in gcc clang; do
    for mode in normal sanitize; do
        flags=()
        if [[ $mode == sanitize ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer); fi
        "$compiler" -std=c11 -O2 -g -Wall -Wextra -Werror -Ilib/bearssl/inc -Ilib/bearssl/src \
            "${flags[@]}" "${sources[@]}" -o "$out/$compiler-$mode"
        "$out/$compiler-$mode"
    done
done
