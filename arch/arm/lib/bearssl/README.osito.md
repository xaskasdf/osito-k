# BearSSL for certificate checks (AArch64)

Unmodified subset of BearSSL 0.6, Copyright Thomas Pornin, MIT license
(`LICENSE.txt`).
Source: https://bearssl.org/bearssl-0.6.tar.gz
Archive SHA256: `6705bba1714961b41a728dfc5debbe348d2966c117649392f8c8139efc83ff14`.

The TLS clients (`kernel/tls13.c`, `kernel/tls.c`) authenticate servers
through `kernel/tls_verify.c`, which uses:
- the X.509 "minimal" engine (`src/x509`): it walks the chain up to a
  trust anchor, checking signatures, CA flags, path lengths, dates and the
  server name;
- RSA PKCS#1 v1.5 verification and the RSA public operation (`src/rsa`, `src/int`, i31);
- ECDSA verification on P-256, P-384 and P-521 (`src/ec`);
- SHA-224 through SHA-512 (`src/hash`).

BearSSL 0.6 has no RSA-PSS, which TLS 1.3 requires of RSA servers, so
`tls_verify.c` implements EMSA-PSS-VERIFY over `br_rsa_i31_public`. The
trust anchors are Mozilla's roots, generated into `kernel/ca_roots.c` by
`tools/ca-roots/gen-bearssl-anchors.py`.

The `src` files are exactly the ones the kernel links: the Makefile builds
everything under `src/*/` into `libbearssl.a` and the linker pulls what is
referenced, so after a change `build/kernel.map` lists the members actually
used. All of `inc/` is kept, since `bearssl.h` includes every header.

`port/string.h` stands in for the toolchain's missing `<string.h>`:
`memcpy`, `memmove` and `memset` come from `kernel/mem.S`, and `memcmp` and
`strlen` from `port/string.c`. The build turns off `time()`, RDRAND, the
x86, SSE and POWER8 code, and unaligned type punning (see `BEARSSL_CFLAGS`
in the Makefile). No local changes to the imported files.
