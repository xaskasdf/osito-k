# BearSSL Primitives for WLAN, Win32 and TLS certificate checks

Unmodified subset of BearSSL 0.6, Copyright Thomas Pornin, MIT license.
Source: https://bearssl.org/bearssl-0.6.tar.gz
Archive SHA256: `6705bba1714961b41a728dfc5debbe348d2966c117649392f8c8139efc83ff14`.

SHA-1, HMAC and constant-time bitsliced AES decryption serve WLAN. The Win32
CryptoAPI hash adapter also uses MD5, SHA-256, SHA-384 and SHA-512. Their
unmodified hash implementations and endian codecs come from the same pinned
archive. The upstream headers are kept intact to preserve their contracts;
this does not enable BearSSL TLS, certificate handling or its RNG. The existing
kernel TLS implementation is unchanged. WLAN uses SHA-1 for the WPA2-PSK
protocol's HMAC/PBKDF2. Win32 callers select their hash algorithm explicitly;
this adapter does not make a certificate trust decision.

`port/string.h` binds the freestanding build to kernel memory helpers. The host
tests use libc instead. Build flags disable unaligned type punning and SIMD;
there are no local changes to the imported implementation files.
The consumer supplies a signature-correct SHA-1 vtable adapter in
`kernel/wpa2_crypto.c`: this avoids BearSSL 0.6's generic function-pointer casts
without disabling Clang's function-type sanitizer. AES is called directly.
The Win32 adapter also calls the typed hash functions directly.

## TLS certificate checks (2026-10-30)

The TLS clients (`kernel/tls13.c`, `kernel/tls.c`) authenticate servers
through `kernel/tls_verify.c`, which uses more of the same archive, still
unmodified (`TLS_CRYPTO_SRC` in the Makefile):
- the X.509 "minimal" engine (`src/x509`): the chain up to a trust anchor,
  with signatures, CA flags, path lengths, dates and the server name;
- RSA PKCS#1 v1.5 verification and the RSA public operation (`src/rsa`, `src/int`, i31);
- ECDSA verification on P-256, P-384 and P-521 (`src/ec`), which
  `kernel/ecdh_p256.c` also uses for P-256 ECDHE;
- `src/hash/multihash.c` and `src/codec/ccopy.c`.

BearSSL 0.6 has no RSA-PSS, which TLS 1.3 requires of RSA servers, so
`tls_verify.c` implements EMSA-PSS-VERIFY over `br_rsa_i31_public`. The
trust anchors are Mozilla's roots, generated into `kernel/ca_roots.c` by
`tools/ca-roots/gen-bearssl-anchors.py`. `port/string.h` declares
`kernel/memory.c`'s `memmove`, which the X.509 engine and i31 use. The
compile rule turns off BearSSL's `time()`, `/dev/urandom` and RDRAND code:
the engine gets the time from `tls_verify.c`, and randomness comes from
`kernel/random_drbg.c`. The same subset serves arch/arm.
