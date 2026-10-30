# BearSSL Primitives for WLAN and Win32

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
